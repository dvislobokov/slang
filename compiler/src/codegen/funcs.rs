//! Function bodies: locals, parameters, the single exit block, `defer`,
//! closures and function-value thunks.

use super::{CodeGen, LocalSlot, Val, R};
use crate::hir::{self, CaptureSource, FuncId, FuncKind, LocalId};
use crate::types::TypeId;
use inkwell::values::{BasicMetadataValueEnum, BasicValueEnum, FunctionValue, PointerValue};
use inkwell::IntPredicate;

/// Maximum number of pending deferred calls per function activation.
pub(crate) const MAX_DEFERS: u32 = 64;

impl<'a, 'ctx> CodeGen<'a, 'ctx> {
    /// Emits the body of HIR function `fid`.
    pub(crate) fn emit_function(&mut self, fid: FuncId) -> R<()> {
        let f: &'a hir::Function = &self.prog.funcs[fid.0 as usize];
        let fv = self.funcs[fid.0 as usize];
        let saved = self.begin_fn(fv, Some(f));
        let off = if matches!(f.kind, FuncKind::Lambda { .. }) { 1 } else { 0 };
        if off == 1 {
            self.fs_mut().env = Some(fv.get_nth_param(0).unwrap().into_pointer_value());
        }
        if f.is_async {
            self.coro_prologue(f)?;
        }
        // Local slots (zeroed at entry).
        let mut slots = Vec::with_capacity(f.locals.len());
        for l in &f.locals {
            let cell = l.captured || l.address_taken;
            let ty = if cell { self.ptr_ty().into() } else { self.llty(l.ty) };
            let p = self.entry_alloca(ty, &l.name, true);
            slots.push(Some(LocalSlot { ptr: p, cell }));
        }
        self.fs_mut().locals = slots;
        if f.ret != self.prog.types.void {
            let t = self.llty(f.ret);
            let p = self.entry_alloca(t, "ret", true);
            self.fs_mut().ret_slot = Some((p, f.ret));
        }
        let exit = self.block("exit");
        self.fs_mut().exit = Some(exit);
        for (k, p) in f.params.iter().enumerate() {
            let v = fv.get_nth_param((k + off) as u32).unwrap();
            self.assign_local(*p, Val::borrowed(v), true)?;
        }
        self.block_stmts(&f.body)?;
        self.br(exit);
        self.b.position_at_end(exit);
        self.emit_exit()?;
        self.end_fn(saved, None);
        Ok(())
    }

    /// The exit block: deferred calls, release of locals, return.
    fn emit_exit(&mut self) -> R<()> {
        self.run_defers();
        self.release_locals();
        if self.fs().coro.is_some() {
            return self.coro_return();
        }
        match self.fs().ret_slot {
            Some((p, t)) => {
                let ty = self.llty(t);
                let v = self.b.build_load(ty, p, "ret").unwrap();
                self.b.build_return(Some(&v)).unwrap();
            }
            None => {
                self.b.build_return(None).unwrap();
            }
        }
        Ok(())
    }

    /// Releases every local slot and scoped temporary of the function.
    pub(crate) fn release_locals(&mut self) {
        let Some(f) = self.fs().hir else { return };
        let slots = self.fs().locals.clone();
        for (i, slot) in slots.iter().enumerate() {
            let Some(slot) = slot else { continue };
            let l = &f.locals[i];
            if slot.cell {
                if !l.address_taken {
                    let c = self.b.build_load(self.ptr_ty(), slot.ptr, "cell").unwrap();
                    self.call_rt("slang_release", &[c.into()]);
                }
            } else if self.needs_rc(l.ty) {
                let ty = self.llty(l.ty);
                let v = self.b.build_load(ty, slot.ptr, "").unwrap();
                self.release_val(l.ty, v);
            }
        }
        let scoped = self.fs().scoped.clone();
        for (p, t) in scoped {
            let ty = self.llty(t);
            let v = self.b.build_load(ty, p, "").unwrap();
            self.release_val(t, v);
        }
    }

    /// Runs the pending deferred closures, last first.
    fn run_defers(&mut self) {
        let Some((arr, n)) = self.fs().defers else { return };
        let check = self.block("defer.check");
        let run = self.block("defer.run");
        let done = self.block("defer.done");
        self.b.build_unconditional_branch(check).unwrap();
        self.b.position_at_end(check);
        let cnt = self.b.build_load(self.i64_ty(), n, "n").unwrap().into_int_value();
        let empty = self.b.build_int_compare(IntPredicate::EQ, cnt, self.i64_ty().const_zero(), "").unwrap();
        self.b.build_conditional_branch(empty, done, run).unwrap();
        self.b.position_at_end(run);
        let n1 = self.b.build_int_sub(cnt, self.i64_ty().const_int(1, false), "").unwrap();
        self.b.build_store(n, n1).unwrap();
        let pair = self.pair_ty();
        let slot = unsafe { self.b.build_in_bounds_gep(pair.array_type(MAX_DEFERS), arr, &[self.i64_ty().const_zero(), n1], "") }.unwrap();
        let c = self.b.build_load(pair, slot, "").unwrap().into_struct_value();
        let fnp = self.b.build_extract_value(c, 0, "").unwrap().into_pointer_value();
        let env = self.b.build_extract_value(c, 1, "").unwrap();
        let fty = self.ctx.void_type().fn_type(&[self.ptr_ty().into()], false);
        self.b.build_indirect_call(fty, fnp, &[env.into()], "").unwrap();
        self.call_rt("slang_release", &[env.into()]);
        self.b.build_unconditional_branch(check).unwrap();
        self.b.position_at_end(done);
    }

    /// `defer closure`.
    pub(crate) fn emit_defer(&mut self, e: &hir::Expr) -> R<()> {
        if self.fs().hir.is_none() {
            return super::unsupported("defer outside a function body");
        }
        let (arr, n) = match self.fs().defers {
            Some(d) => d,
            None => {
                // Allocated on first use; the exit block is emitted last.
                let pair = self.pair_ty();
                let arr = self.entry_alloca(pair.array_type(MAX_DEFERS).into(), "defers", false);
                let n = self.entry_alloca(self.i64_ty().into(), "ndefers", true);
                self.fs_mut().defers = Some((arr, n));
                (arr, n)
            }
        };
        let c = self.expr_owned(e)?;
        let cnt = self.b.build_load(self.i64_ty(), n, "n").unwrap().into_int_value();
        let full = self.b.build_int_compare(IntPredicate::UGE, cnt, self.i64_ty().const_int(MAX_DEFERS as u64, false), "").unwrap();
        let bad = self.block("defer.full");
        let ok = self.block("defer.ok");
        self.b.build_conditional_branch(full, bad, ok).unwrap();
        self.b.position_at_end(bad);
        let msg = self.cstr("too many pending deferred calls (limit 64)");
        self.call_noreturn("slang_panic", &[msg.into()]);
        self.br(ok);
        self.b.position_at_end(ok);
        let pair = self.pair_ty();
        let slot = unsafe { self.b.build_in_bounds_gep(pair.array_type(MAX_DEFERS), arr, &[self.i64_ty().const_zero(), cnt], "") }.unwrap();
        self.b.build_store(slot, c).unwrap();
        let n1 = self.b.build_int_add(cnt, self.i64_ty().const_int(1, false), "").unwrap();
        self.b.build_store(n, n1).unwrap();
        Ok(())
    }

    // ------------------------------------------------------------------
    // Locals and cells
    // ------------------------------------------------------------------

    fn local_info(&self, l: LocalId) -> (&'a hir::Local, LocalSlot<'ctx>) {
        let f = self.fs().hir.expect("codegen: local outside a function");
        (&f.locals[l.0 as usize], self.fs().locals[l.0 as usize].expect("codegen: local without slot"))
    }

    /// Address of the value of local `l`.
    pub(crate) fn local_addr(&mut self, l: LocalId) -> PointerValue<'ctx> {
        let (_, slot) = self.local_info(l);
        if slot.cell {
            let c = self.b.build_load(self.ptr_ty(), slot.ptr, "cell").unwrap().into_pointer_value();
            self.payload(c)
        } else {
            slot.ptr
        }
    }

    /// Address of upvalue `i` of the current lambda.
    pub(crate) fn upvalue_addr(&mut self, i: u32) -> PointerValue<'ctx> {
        let cell = self.upvalue_cell(i);
        self.payload(cell)
    }

    fn upvalue_cell(&mut self, i: u32) -> PointerValue<'ctx> {
        let env = self.fs().env.expect("codegen: upvalue outside a lambda");
        let p = self.byte_gep(env, 24 + 8 * i as u64);
        self.b.build_load(self.ptr_ty(), p, "upcell").unwrap().into_pointer_value()
    }

    /// `obj + 16`: the payload of a heap object.
    pub(crate) fn payload(&mut self, obj: PointerValue<'ctx>) -> PointerValue<'ctx> {
        self.byte_gep(obj, 16)
    }

    pub(crate) fn byte_gep(&mut self, p: PointerValue<'ctx>, off: u64) -> PointerValue<'ctx> {
        unsafe { self.b.build_in_bounds_gep(self.ctx.i8_type(), p, &[self.i64_ty().const_int(off, false)], "") }.unwrap()
    }

    /// Stores `v` into local `l`. With `fresh` (declaration, loop variable,
    /// binding, parameter) a captured local gets a new cell.
    pub(crate) fn assign_local(&mut self, l: LocalId, v: Val<'ctx>, fresh: bool) -> R<()> {
        let (info, slot) = self.local_info(l);
        if slot.cell && fresh {
            let owned = self.to_owned(info.ty, v);
            let cell = self.new_box(info.ty, owned);
            let old = self.b.build_load(self.ptr_ty(), slot.ptr, "").unwrap();
            if !info.address_taken {
                self.call_rt("slang_release", &[old.into()]);
            }
            self.b.build_store(slot.ptr, cell).unwrap();
        } else {
            let p = self.local_addr(l);
            self.store_assign(p, info.ty, v);
        }
        Ok(())
    }

    /// Resets local `l` to its zero value (`var x T` executed again).
    pub(crate) fn zero_local(&mut self, l: LocalId) -> R<()> {
        let (info, _) = self.local_info(l);
        let z = self.llty(info.ty).const_zero();
        self.assign_local(l, Val::borrowed(z), true)
    }

    // ------------------------------------------------------------------
    // Closures
    // ------------------------------------------------------------------

    /// Shared type info of closure environments: `{hdr, i64 n, ptr cells[n]}`.
    fn env_type_info(&mut self) -> PointerValue<'ctx> {
        if let Some(g) = self.env_ti {
            return g.as_pointer_value();
        }
        let fty = self.ctx.void_type().fn_type(&[self.ptr_ty().into()], false);
        let drop = self.module.add_function("slang.env.drop", fty, Some(inkwell::module::Linkage::Internal));
        let _ = self.with_helper(drop, |cg| {
            let obj = drop.get_nth_param(0).unwrap().into_pointer_value();
            let np = cg.byte_gep(obj, 16);
            let n = cg.b.build_load(cg.i64_ty(), np, "n").unwrap().into_int_value();
            let cells = cg.byte_gep(obj, 24);
            let i = cg.entry_alloca(cg.i64_ty().into(), "i", true);
            let head = cg.block("head");
            let body = cg.block("body");
            let done = cg.block("done");
            cg.b.build_unconditional_branch(head).unwrap();
            cg.b.position_at_end(head);
            let iv = cg.b.build_load(cg.i64_ty(), i, "").unwrap().into_int_value();
            let c = cg.b.build_int_compare(IntPredicate::ULT, iv, n, "").unwrap();
            cg.b.build_conditional_branch(c, body, done).unwrap();
            cg.b.position_at_end(body);
            let cp = unsafe { cg.b.build_in_bounds_gep(cg.ptr_ty(), cells, &[iv], "") }.unwrap();
            let cell = cg.b.build_load(cg.ptr_ty(), cp, "").unwrap();
            cg.call_rt("slang_release", &[cell.into()]);
            let i1 = cg.b.build_int_add(iv, cg.i64_ty().const_int(1, false), "").unwrap();
            cg.b.build_store(i, i1).unwrap();
            cg.b.build_unconditional_branch(head).unwrap();
            cg.b.position_at_end(done);
            cg.b.build_return(None).unwrap();
            Ok(())
        });
        let name = self.cstr("closure env");
        let tity = self.type_info_ty();
        let init = tity.const_named_struct(&[name.into(), self.i64_ty().const_zero().into(), drop.as_global_value().as_pointer_value().into()]);
        let g = self.module.add_global(tity, None, "ti.env");
        g.set_linkage(inkwell::module::Linkage::Internal);
        g.set_constant(true);
        g.set_initializer(&init);
        self.env_ti = Some(g);
        g.as_pointer_value()
    }

    /// `closure func [captures]`: an owned `{fn, env}`.
    pub(crate) fn emit_closure(&mut self, func: FuncId, captures: &[CaptureSource]) -> R<Val<'ctx>> {
        let fv = self.funcs[func.0 as usize];
        let env = if captures.is_empty() {
            self.ptr_ty().const_null()
        } else {
            let ti = self.env_type_info();
            let size = self.i64_ty().const_int(24 + 8 * captures.len() as u64, false);
            let obj = self.call_rt_v("slang_object_new", &[ti.into(), size.into()]).into_pointer_value();
            let np = self.byte_gep(obj, 16);
            self.b.build_store(np, self.i64_ty().const_int(captures.len() as u64, false)).unwrap();
            for (i, c) in captures.iter().enumerate() {
                let cell = match c {
                    CaptureSource::Local(l) => {
                        let (_, slot) = self.local_info(*l);
                        if !slot.cell {
                            return super::unsupported("capture of a local without a cell");
                        }
                        self.b.build_load(self.ptr_ty(), slot.ptr, "").unwrap().into_pointer_value()
                    }
                    CaptureSource::Upvalue(u) => self.upvalue_cell(*u),
                };
                self.call_rt("slang_retain", &[cell.into()]);
                let p = self.byte_gep(obj, 24 + 8 * i as u64);
                self.b.build_store(p, cell).unwrap();
            }
            obj
        };
        let v = self.make_pair(fv.as_global_value().as_pointer_value(), env);
        Ok(Val::owned(v))
    }

    pub(crate) fn make_pair(&mut self, a: PointerValue<'ctx>, b: PointerValue<'ctx>) -> BasicValueEnum<'ctx> {
        let pt = self.pair_ty();
        let s = self.b.build_insert_value(pt.get_undef(), a, 0, "").unwrap();
        self.b.build_insert_value(s, b, 1, "").unwrap().into_struct_value().into()
    }

    /// A named function used as a value: `{thunk, null}`.
    pub(crate) fn func_ref(&mut self, fid: FuncId, fty: TypeId) -> R<Val<'ctx>> {
        let thunk = self.func_thunk(fid, fty)?;
        let v = self.make_pair(thunk.as_global_value().as_pointer_value(), self.ptr_ty().const_null());
        Ok(Val::borrowed(v))
    }

    fn func_thunk(&mut self, fid: FuncId, fty: TypeId) -> R<FunctionValue<'ctx>> {
        if let Some(t) = self.thunks.get(&fid) {
            return Ok(*t);
        }
        let f = &self.prog.funcs[fid.0 as usize];
        if matches!(f.kind, FuncKind::Lambda { .. }) {
            return super::unsupported("a lambda referenced by name");
        }
        let lty = self.closure_fn_llty(fty);
        let thunk = self.module.add_function(&format!("{}$thunk", f.symbol), lty, Some(inkwell::module::Linkage::Internal));
        self.thunks.insert(fid, thunk);
        let target = self.funcs[fid.0 as usize];
        self.with_helper(thunk, |cg| {
            let args: Vec<BasicMetadataValueEnum> = thunk.get_params().iter().skip(1).map(|p| (*p).into()).collect();
            let r = cg.b.build_call(target, &args, "").unwrap().try_as_basic_value().left();
            match r {
                Some(v) => cg.b.build_return(Some(&v)).unwrap(),
                None => cg.b.build_return(None).unwrap(),
            };
            Ok(())
        })?;
        Ok(thunk)
    }

    /// Emits helper function `fv` with `body`, saving and restoring the
    /// current function state and insertion point.
    pub(crate) fn with_helper<T>(&mut self, fv: FunctionValue<'ctx>, body: impl FnOnce(&mut Self) -> R<T>) -> R<T> {
        let pos = self.b.get_insert_block();
        let saved = self.begin_fn(fv, None);
        let r = body(self);
        self.end_fn(saved, pos);
        r
    }

    /// Calls closure value `c` (of Slang type `fty`) with `args`.
    pub(crate) fn call_closure(&mut self, fty: TypeId, c: BasicValueEnum<'ctx>, args: &[BasicValueEnum<'ctx>]) -> R<Option<BasicValueEnum<'ctx>>> {
        let cs = c.into_struct_value();
        let fnp = self.b.build_extract_value(cs, 0, "fn").unwrap().into_pointer_value();
        let env = self.b.build_extract_value(cs, 1, "env").unwrap();
        // Calling a nil func value panics.
        self.nil_check(fnp);
        let lty = self.closure_fn_llty(fty);
        let mut all: Vec<BasicMetadataValueEnum> = vec![env.into()];
        all.extend(args.iter().map(|a| BasicMetadataValueEnum::from(*a)));
        Ok(self.b.build_indirect_call(lty, fnp, &all, "").unwrap().try_as_basic_value().left())
    }

    /// Panics with `slang_panic_nil` if `p` is NULL.
    pub(crate) fn nil_check(&mut self, p: PointerValue<'ctx>) {
        let isnull = self.b.build_is_null(p, "isnil").unwrap();
        let bad = self.block("nil");
        let ok = self.block("notnil");
        self.b.build_conditional_branch(isnull, bad, ok).unwrap();
        self.b.position_at_end(bad);
        self.call_rt("slang_panic_nil", &[]);
        self.b.build_unreachable().unwrap();
        self.b.position_at_end(ok);
    }

}
