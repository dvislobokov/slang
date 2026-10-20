//! Interfaces, `any`, typed errors, type switches and `errors.As`.

use super::{unsupported, CodeGen, Val, R};
use crate::hir::{Expr, FuncId};
use crate::types::{Type, TypeId};
use inkwell::module::Linkage;
use inkwell::types::{BasicMetadataTypeEnum, BasicType};
use inkwell::values::{BasicMetadataValueEnum, BasicValueEnum, FunctionValue, IntValue, PointerValue};
use inkwell::IntPredicate;

impl<'a, 'ctx> CodeGen<'a, 'ctx> {
    /// Finds method `name` of struct `sid`, possibly promoted through
    /// embedded fields: (function, path of embedded field indices).
    pub(crate) fn resolve_method(&self, sid: u32, name: &str) -> Option<(FuncId, Vec<u32>)> {
        let s = &self.prog.structs[sid as usize];
        if let Some((_, f)) = s.methods.iter().find(|(n, _)| n == name) {
            return Some((*f, Vec::new()));
        }
        for (i, fd) in s.fields.iter().enumerate() {
            if !fd.embedded {
                continue;
            }
            if let Type::Struct(d) = self.ty(fd.ty) {
                if let Some((f, mut path)) = self.resolve_method(d.0, name) {
                    path.insert(0, i as u32);
                    return Some((f, path));
                }
            }
        }
        None
    }

    /// A function `(ptr this, args...)` calling method `name` of `sid`
    /// (with receiver adjustment for promoted methods).
    fn method_entry(&mut self, sid: u32, name: &str) -> R<FunctionValue<'ctx>> {
        let Some((fid, path)) = self.resolve_method(sid, name) else {
            return unsupported(format!("method {name} not found on {}", self.prog.structs[sid as usize].full_name));
        };
        let target = self.funcs[fid.0 as usize];
        if path.is_empty() {
            return Ok(target);
        }
        let key = (sid, name.to_string());
        if let Some(f) = self.method_thunks.get(&key) {
            return Ok(*f);
        }
        let fty = target.get_type();
        let n = self.fresh();
        let thunk = self.module.add_function(&format!("method.thunk.{n}"), fty, Some(Linkage::Internal));
        self.method_thunks.insert(key, thunk);
        self.with_helper(thunk, |cg| {
            let mut this = thunk.get_nth_param(0).unwrap().into_pointer_value();
            let mut cur = sid;
            for idx in &path {
                let st = cg.struct_llty(cur);
                this = cg.b.build_struct_gep(st, this, *idx, "").unwrap();
                let ft = cg.prog.structs[cur as usize].fields[*idx as usize].ty;
                cur = match cg.ty(ft) {
                    Type::Struct(d) => d.0,
                    _ => cur,
                };
            }
            let mut args: Vec<BasicMetadataValueEnum> = vec![this.into()];
            args.extend(thunk.get_params().iter().skip(1).map(|p| BasicMetadataValueEnum::from(*p)));
            let r = cg.b.build_call(target, &args, "").unwrap().try_as_basic_value().left();
            match r {
                Some(v) => cg.b.build_return(Some(&v)).unwrap(),
                None => cg.b.build_return(None).unwrap(),
            };
            Ok(())
        })?;
        Ok(thunk)
    }

    /// Vtable of dynamic type `dyn_ty` (`S` or `*S`) for interface `iface`.
    fn vtable(&mut self, dyn_ty: TypeId, sid: u32, iface: TypeId) -> R<PointerValue<'ctx>> {
        if let Some(g) = self.vtables.get(&(dyn_ty, iface)) {
            return Ok(g.as_pointer_value());
        }
        let Type::Interface(d) = self.ty(iface) else { return unsupported("vtable for a non-interface") };
        let names: Vec<String> = self.prog.interfaces[d.0 as usize].methods.iter().map(|m| m.name.clone()).collect();
        let boxed = !matches!(self.ty(dyn_ty), Type::Pointer(_));
        let tid = self.type_info(dyn_ty);
        let mut vals: Vec<BasicValueEnum> = vec![tid.into(), self.i64_ty().const_int(boxed as u64, false).into()];
        for n in &names {
            let f = self.method_entry(sid, n)?;
            vals.push(f.as_global_value().as_pointer_value().into());
        }
        let tys: Vec<inkwell::types::BasicTypeEnum> = vals.iter().map(|v| v.get_type()).collect();
        let st = self.ctx.struct_type(&tys, false);
        let k = self.fresh();
        let g = self.module.add_global(st, None, &format!("vt.{k}"));
        g.set_linkage(Linkage::Internal);
        g.set_constant(true);
        g.set_initializer(&st.const_named_struct(&vals));
        self.vtables.insert((dyn_ty, iface), g);
        Ok(g.as_pointer_value())
    }

    /// `slang_message_fn` thunk calling `S.Message()` on a box payload.
    fn message_thunk(&mut self, sid: u32) -> R<FunctionValue<'ctx>> {
        if let Some(f) = self.msg_thunks.get(&sid) {
            return Ok(*f);
        }
        let m = self.method_entry(sid, "Message")?;
        let fty = self.ptr_ty().fn_type(&[self.ptr_ty().into()], false);
        let k = self.fresh();
        let f = self.module.add_function(&format!("msg.thunk.{k}"), fty, Some(Linkage::Internal));
        self.msg_thunks.insert(sid, f);
        self.with_helper(f, |cg| {
            let obj = f.get_nth_param(0).unwrap().into_pointer_value();
            let p = cg.payload(obj);
            let r = cg.b.build_call(m, &[p.into()], "").unwrap().try_as_basic_value().left().unwrap();
            cg.b.build_return(Some(&r)).unwrap();
            Ok(())
        })?;
        Ok(f)
    }

    /// Boxes struct value `v` (owned) as a typed error (+1).
    fn box_error(&mut self, st: TypeId, sid: u32, v: BasicValueEnum<'ctx>) -> R<BasicValueEnum<'ctx>> {
        let thunk = self.message_thunk(sid)?;
        let obj = self.new_box(st, v);
        let ti = self.type_info(st);
        let e = self.call_rt_v("slang_error_new_typed", &[ti.into(), obj.into(), thunk.as_global_value().as_pointer_value().into()]);
        self.call_rt("slang_release", &[obj.into()]);
        Ok(e)
    }

    /// `ToInterface`: box `value` into interface/error/any type `to`.
    pub(crate) fn to_interface(&mut self, value: &Expr, to: TypeId) -> R<Val<'ctx>> {
        let from = value.ty;
        match (self.ty(to).clone(), self.ty(from).clone()) {
            (Type::Error, Type::Error) => self.expr(value),
            (Type::Error, Type::Struct(d)) => {
                let v = self.expr_owned(value)?;
                Ok(Val::owned(self.box_error(from, d.0, v)?))
            }
            (Type::Error, Type::Pointer(inner)) => {
                let Type::Struct(d) = self.ty(inner).clone() else { return unsupported("non-struct pointer as error") };
                let pv = self.expr(value)?;
                let p = pv.get().into_pointer_value();
                self.nil_check(p);
                let lt = self.llty(inner);
                let sv = self.b.build_load(lt, p, "").unwrap();
                self.retain_val(inner, sv);
                let e = self.box_error(inner, d.0, sv)?;
                self.drop_val(from, pv);
                Ok(Val::owned(e))
            }
            (Type::Interface(_), Type::Struct(d)) => {
                let v = self.expr_owned(value)?;
                let vt = self.vtable(from, d.0, to)?;
                let obj = self.new_box(from, v);
                let data = self.payload(obj);
                Ok(Val::owned(self.make_pair(data, vt)))
            }
            (Type::Interface(_), Type::Pointer(inner)) => {
                let Type::Struct(d) = self.ty(inner).clone() else { return unsupported("non-struct pointer in an interface") };
                let pv = self.expr(value)?;
                let p = pv.get().into_pointer_value();
                let vt = self.vtable(from, d.0, to)?;
                // The interface value owns the pointer like a box.
                Ok(Val { v: Some(self.make_pair(p, vt)), owned: pv.owned })
            }
            (Type::Interface(_), Type::Interface(_)) if from == to => self.expr(value),
            (Type::Any, Type::Any) => self.expr(value),
            (Type::Any, _) => {
                let v = self.expr_owned(value)?;
                let obj = self.new_box(from, v);
                let data = self.payload(obj);
                let ti = self.type_info(from);
                Ok(Val::owned(self.make_pair(data, ti)))
            }
            _ => unsupported(format!("conversion of {} to {}", self.type_name(from), self.type_name(to))),
        }
    }

    /// Dynamic call of interface method `method` on `recv`.
    pub(crate) fn iface_call(&mut self, ty: TypeId, recv: &Expr, method: u32, args: &[Expr]) -> R<Val<'ctx>> {
        if *self.ty(recv.ty) == Type::Error {
            let rv = self.expr(recv)?;
            let r = self.call_rt_v("slang_error_message", &[rv.get().into()]);
            self.drop_val(recv.ty, rv);
            return Ok(Val::owned(r));
        }
        let dep = self.depth();
        let (rv, _) = self.arg(recv, false, true, false)?;
        let Type::Interface(d) = self.ty(recv.ty).clone() else { return unsupported("method call on this type") };
        let sig = self.prog.interfaces[d.0 as usize].methods[method as usize].clone();
        let pair = rv.get().into_struct_value();
        let data = self.b.build_extract_value(pair, 0, "").unwrap();
        let vt = self.b.build_extract_value(pair, 1, "").unwrap().into_pointer_value();
        self.nil_check(vt);
        let fp = self.byte_gep(vt, 16 + 8 * method as u64);
        let fnp = self.b.build_load(self.ptr_ty(), fp, "").unwrap().into_pointer_value();
        let mut ps: Vec<BasicMetadataTypeEnum> = vec![self.ptr_ty().into()];
        for p in &sig.params {
            ps.push(self.llty(*p).into());
        }
        let fty = match self.llty_opt(sig.ret) {
            Some(t) => t.fn_type(&ps, false),
            None => self.ctx.void_type().fn_type(&ps, false),
        };
        let mut vals = Vec::new();
        let mut moved_holds = Vec::new();
        for (k, a) in args.iter().enumerate() {
            let moved = sig.params.get(k).is_some_and(|t| self.has_dtor(*t));
            let (v, h) = self.arg(a, moved, true, false)?;
            if moved {
                moved_holds.push(h);
            }
            vals.push(v);
        }
        let mut ma: Vec<BasicMetadataValueEnum> = vec![data.into()];
        ma.extend(vals.iter().map(|v| BasicMetadataValueEnum::from(v.get())));
        let r = self.b.build_indirect_call(fty, fnp, &ma, "").unwrap().try_as_basic_value().left();
        for h in moved_holds {
            self.unhold(h);
        }
        self.release_temps_from(dep);
        let _ = ty;
        Ok(Val { v: r, owned: true })
    }

    /// For a type-switch subject `subj` (interface or error value) and
    /// pattern type `pat`: (matches, pointer to the stored `pat` value).
    pub(crate) fn type_test(&mut self, subj_ty: TypeId, subj: BasicValueEnum<'ctx>, pat: TypeId) -> R<(IntValue<'ctx>, PointerValue<'ctx>)> {
        let ti = self.type_info(pat);
        let res = self.entry_alloca(self.ctx.bool_type().into(), "tm", false);
        self.b.build_store(res, self.ctx.bool_type().const_zero()).unwrap();
        let datap = self.entry_alloca(self.ptr_ty().into(), "td", false);
        self.b.build_store(datap, self.ptr_ty().const_null()).unwrap();
        let has = self.block("tt.has");
        let done = self.block("tt.done");
        match self.ty(subj_ty) {
            Type::Error => {
                let e = subj.into_pointer_value();
                let nn = self.b.build_is_not_null(e, "").unwrap();
                self.b.build_conditional_branch(nn, has, done).unwrap();
                self.b.position_at_end(has);
                let utp = self.byte_gep(e, 32);
                let ut = self.b.build_load(self.ptr_ty(), utp, "").unwrap().into_pointer_value();
                let eq = self.ptr_eq(ut, ti);
                self.b.build_store(res, eq).unwrap();
                let pp = self.byte_gep(e, 40);
                let payload = self.b.build_load(self.ptr_ty(), pp, "").unwrap().into_pointer_value();
                let d = self.payload(payload);
                if matches!(self.ty(pat), Type::Pointer(_)) {
                    return unsupported("pointer type patterns on errors");
                }
                self.b.build_store(datap, d).unwrap();
            }
            Type::Interface(_) | Type::Any => {
                let pair = subj.into_struct_value();
                let data = self.b.build_extract_value(pair, 0, "").unwrap().into_pointer_value();
                let vt = self.b.build_extract_value(pair, 1, "").unwrap().into_pointer_value();
                let nn = self.b.build_is_not_null(vt, "").unwrap();
                self.b.build_conditional_branch(nn, has, done).unwrap();
                self.b.position_at_end(has);
                let tid = if matches!(self.ty(subj_ty), Type::Any) {
                    vt
                } else {
                    self.b.build_load(self.ptr_ty(), vt, "").unwrap().into_pointer_value()
                };
                let eq = self.ptr_eq(tid, ti);
                self.b.build_store(res, eq).unwrap();
                if matches!(self.ty(pat), Type::Pointer(_)) && !matches!(self.ty(subj_ty), Type::Any) {
                    // An interface stores a `*S` itself (an `any` boxes it):
                    // the binding is the data pointer, so store its address.
                    let slot = self.entry_alloca(self.ptr_ty().into(), "tp", false);
                    self.b.build_store(slot, data).unwrap();
                    self.b.build_store(datap, slot).unwrap();
                } else {
                    self.b.build_store(datap, data).unwrap();
                }
            }
            _ => return unsupported("type switch on this type"),
        }
        self.b.build_unconditional_branch(done).unwrap();
        self.b.position_at_end(done);
        let m = self.b.build_load(self.ctx.bool_type(), res, "").unwrap().into_int_value();
        let d = self.b.build_load(self.ptr_ty(), datap, "").unwrap().into_pointer_value();
        Ok((m, d))
    }

    pub(crate) fn ptr_eq(&mut self, a: PointerValue<'ctx>, b: PointerValue<'ctx>) -> IntValue<'ctx> {
        let ai = self.b.build_ptr_to_int(a, self.i64_ty(), "").unwrap();
        let bi = self.b.build_ptr_to_int(b, self.i64_ty(), "").unwrap();
        self.b.build_int_compare(IntPredicate::EQ, ai, bi, "").unwrap()
    }

    /// `errors.As(err, target)`.
    pub(crate) fn errors_as(&mut self, err: &Expr, target: &Expr) -> R<Val<'ctx>> {
        let Type::Pointer(st) = self.ty(target.ty).clone() else { return unsupported("errors.As target") };
        let ev = self.expr(err)?;
        let h = self.hold(err.ty, ev);
        let tp = self.expr(target)?.get().into_pointer_value();
        self.unhold(h);
        let ti = self.type_info(st);
        let payload = self.call_rt_v("slang_error_as", &[ev.get().into(), ti.into()]).into_pointer_value();
        let found = self.b.build_is_not_null(payload, "").unwrap();
        let yes = self.block("as.yes");
        let done = self.block("as.done");
        self.b.build_conditional_branch(found, yes, done).unwrap();
        self.b.position_at_end(yes);
        self.nil_check(tp);
        let src = self.payload(payload);
        let lt = self.llty(st);
        let v = self.b.build_load(lt, src, "").unwrap();
        self.store_assign(tp, st, Val::borrowed(v));
        self.b.build_unconditional_branch(done).unwrap();
        self.b.position_at_end(done);
        self.drop_val(err.ty, ev);
        Ok(Val::borrowed(found.into()))
    }
}
