//! Statements, loops, `switch`, `return` and the error-handling
//! expressions (`?`, `try ... catch`, `try ... else`).

use super::{unsupported, CodeGen, LoopTarget, Val, R};
use crate::hir::{Block, Expr, ExprKind, Label, LocalId, Pattern, ReturnValue, Stmt, StmtKind, Switch, TryFallback};
use crate::types::{Type, TypeId};
use inkwell::basic_block::BasicBlock;
use inkwell::values::{BasicValueEnum, IntValue, PointerValue};
use inkwell::IntPredicate;

impl<'a, 'ctx> CodeGen<'a, 'ctx> {
    pub(crate) fn block_stmts(&mut self, b: &Block) -> R<()> {
        for s in &b.stmts {
            self.stmt(s)?;
        }
        Ok(())
    }

    fn terminated(&self) -> bool {
        self.b.get_insert_block().and_then(|b| b.get_terminator()).is_some()
    }

    /// Branch to `bb`, then continue in a dead block.
    fn jump(&mut self, bb: BasicBlock<'ctx>) {
        self.br(bb);
        self.dead_block();
    }

    fn stmt(&mut self, s: &Stmt) -> R<()> {
        if self.terminated() {
            self.dead_block();
        }
        match &s.kind {
            StmtKind::Let { local, init } => match init {
                Some(e) => {
                    let v = self.expr(e)?;
                    self.assign_local(*local, v, true)
                }
                None => self.zero_local(*local),
            },
            StmtKind::LetTuple { locals, init } => {
                let tv = self.expr_owned(init)?;
                let Type::Tuple(ts) = self.ty(init.ty).clone() else { return unsupported("tuple destructuring of a non-tuple") };
                for (i, l) in locals.iter().enumerate() {
                    let c = self.b.build_extract_value(tv.into_struct_value(), i as u32, "").unwrap();
                    match l {
                        Some(l) => self.assign_local(*l, Val::owned(c), true)?,
                        None => self.release_val(ts[i], c),
                    }
                }
                Ok(())
            }
            StmtKind::Assign { target, value } => self.assign(target, value),
            StmtKind::CompoundAssign { target, op, value } => {
                let rv = self.expr(value)?;
                let p = if let ExprKind::MapGet { map, key } = &target.kind {
                    let (kt, _) = self.map_kv(map.ty);
                    let mv = self.expr(map)?;
                    let kv = self.expr(key)?;
                    let slot = self.map_slot(kt, mv.get(), kv.get(), true);
                    self.drop_val(key.ty, kv);
                    // The map stays alive through its variable (a temporary
                    // map would be a no-op target anyway).
                    if mv.owned {
                        let r = self.compound_at(slot, target.ty, *op, rv, value.ty);
                        self.drop_val(map.ty, mv);
                        return r;
                    }
                    slot
                } else {
                    self.place(target)?
                };
                self.compound_at(p, target.ty, *op, rv, value.ty)
            }
            StmtKind::Expr(e) | StmtKind::Discard(e) => self.expr_discard(e),
            StmtKind::If { cond, then, else_ } => {
                let c = self.expr(cond)?.get().into_int_value();
                let tb = self.block("then");
                let eb = self.block("else");
                let end = self.block("endif");
                self.b.build_conditional_branch(c, tb, eb).unwrap();
                self.b.position_at_end(tb);
                self.block_stmts(then)?;
                self.br(end);
                self.b.position_at_end(eb);
                if let Some(e) = else_ {
                    self.block_stmts(e)?;
                }
                self.br(end);
                self.b.position_at_end(end);
                Ok(())
            }
            StmtKind::Loop { label, cond, body } => {
                let head = self.block("loop.head");
                let bb = self.block("loop.body");
                let end = self.block("loop.end");
                self.br(head);
                self.b.position_at_end(head);
                match cond {
                    Some(c) => {
                        let cv = self.expr(c)?.get().into_int_value();
                        self.b.build_conditional_branch(cv, bb, end).unwrap();
                    }
                    None => {
                        self.b.build_unconditional_branch(bb).unwrap();
                    }
                }
                self.b.position_at_end(bb);
                self.loop_body(*label, end, head, body)?;
                self.b.position_at_end(end);
                Ok(())
            }
            StmtKind::ForRange { label, var, start, end, body } => {
                let sv = self.expr(start)?.get().into_int_value();
                let ev = self.expr(end)?.get().into_int_value();
                let signed = self.is_signed(start.ty);
                let ctr = self.entry_alloca(sv.get_type().into(), "i", false);
                self.b.build_store(ctr, sv).unwrap();
                let head = self.block("for.head");
                let bb = self.block("for.body");
                let cont = self.block("for.next");
                let done = self.block("for.end");
                self.br(head);
                self.b.position_at_end(head);
                let iv = self.b.build_load(sv.get_type(), ctr, "").unwrap().into_int_value();
                let c = self.b.build_int_compare(if signed { IntPredicate::SLT } else { IntPredicate::ULT }, iv, ev, "").unwrap();
                self.b.build_conditional_branch(c, bb, done).unwrap();
                self.b.position_at_end(bb);
                if let Some(v) = var {
                    self.assign_local(*v, Val::borrowed(iv.into()), true)?;
                }
                self.loop_body(*label, done, cont, body)?;
                self.b.position_at_end(cont);
                let iv = self.b.build_load(sv.get_type(), ctr, "").unwrap().into_int_value();
                let n = self.b.build_int_add(iv, sv.get_type().const_int(1, false), "").unwrap();
                self.b.build_store(ctr, n).unwrap();
                self.b.build_unconditional_branch(head).unwrap();
                self.b.position_at_end(done);
                Ok(())
            }
            StmtKind::ForSlice { label, index, elem, seq, body } => self.for_slice(*label, *index, *elem, seq, body),
            StmtKind::ForString { label, index, ch, string, body } => self.for_string(*label, *index, *ch, string, body),
            StmtKind::ForMap { label, key, value, map, body } => self.for_map(*label, *key, *value, map, body),
            StmtKind::Switch(sw) => self.switch(sw),
            StmtKind::Return(rv) => self.emit_return(rv),
            StmtKind::Break(l) => {
                let t = self.target(*l)?;
                self.jump(t.brk);
                Ok(())
            }
            StmtKind::Continue(l) => {
                let t = self.target(*l)?;
                let Some(c) = t.cont else { return unsupported("continue to a switch") };
                self.jump(c);
                Ok(())
            }
            StmtKind::Defer(e) => self.emit_defer(e),
            StmtKind::Block(b) => self.block_stmts(b),
        }
    }

    fn target(&self, l: Label) -> R<LoopTarget<'ctx>> {
        match self.fs().loops.iter().rev().find(|t| t.label == l) {
            Some(t) => Ok(*t),
            None => unsupported("break/continue target outside the function"),
        }
    }

    /// Emits a loop body with its break/continue targets, then branches to
    /// `cont`.
    fn loop_body(&mut self, label: Label, brk: BasicBlock<'ctx>, cont: BasicBlock<'ctx>, body: &Block) -> R<()> {
        self.fs_mut().loops.push(LoopTarget { label, brk, cont: Some(cont) });
        let r = self.block_stmts(body);
        self.fs_mut().loops.pop();
        r?;
        self.br(cont);
        Ok(())
    }

    fn assign(&mut self, target: &Expr, value: &Expr) -> R<()> {
        if let ExprKind::MapGet { map, key } = &target.kind {
            let (kt, vt) = self.map_kv(map.ty);
            let mv = self.expr(map)?;
            let kv = self.expr(key)?;
            let vv = self.expr(value)?;
            self.map_set(kt, vt, mv.get().into_pointer_value(), kv.get(), vv.get())?;
            self.drop_val(value.ty, vv);
            self.drop_val(key.ty, kv);
            self.drop_val(map.ty, mv);
            return Ok(());
        }
        let vv = self.expr(value)?;
        if let ExprKind::Local(l) = &target.kind {
            return self.assign_local(*l, vv, false);
        }
        let p = self.place(target)?;
        self.store_assign(p, target.ty, vv);
        Ok(())
    }

    /// `*p op= rv`.
    fn compound_at(&mut self, p: PointerValue<'ctx>, ty: TypeId, op: crate::hir::BinOp, rv: Val<'ctx>, rty: TypeId) -> R<()> {
        let lt = self.llty(ty);
        let old = self.b.build_load(lt, p, "").unwrap();
        let nv = self.binop_vals(op, ty, old, rv.get(), rty)?;
        let nv_owned = self.to_owned(ty, nv);
        self.b.build_store(p, nv_owned).unwrap();
        self.release_val(ty, old);
        self.drop_val(rty, rv);
        Ok(())
    }

    /// A zeroed slot released at function exit, holding a loop sequence.
    fn scoped_slot(&mut self, t: TypeId, v: Val<'ctx>) -> PointerValue<'ctx> {
        let lt = self.llty(t);
        let p = self.entry_alloca(lt, "seq", true);
        if self.needs_rc(t) {
            self.fs_mut().scoped.push((p, t));
        }
        self.store_assign(p, t, v);
        p
    }

    fn for_slice(&mut self, label: Label, index: Option<LocalId>, elem: Option<LocalId>, seq: &Expr, body: &Block) -> R<()> {
        let sv = self.expr(seq)?;
        let tmp = self.scoped_slot(seq.ty, sv);
        let et = self.elem_of(seq.ty).unwrap();
        let elt = self.llty(et);
        let is_array = matches!(self.ty(seq.ty), Type::Array(..));
        let (data, len): (PointerValue, IntValue) = if let Type::Array(_, n) = self.ty(seq.ty) {
            (tmp, self.i64_ty().const_int(*n, false))
        } else {
            let s = self.b.build_load(self.slice_ty(), tmp, "").unwrap().into_struct_value();
            (self.b.build_extract_value(s, 0, "").unwrap().into_pointer_value(), self.b.build_extract_value(s, 1, "").unwrap().into_int_value())
        };
        let ctr = self.entry_alloca(self.i64_ty().into(), "i", false);
        self.b.build_store(ctr, self.i64_ty().const_zero()).unwrap();
        let head = self.block("fs.head");
        let bb = self.block("fs.body");
        let cont = self.block("fs.next");
        let done = self.block("fs.end");
        self.br(head);
        self.b.position_at_end(head);
        let iv = self.b.build_load(self.i64_ty(), ctr, "").unwrap().into_int_value();
        let c = self.b.build_int_compare(IntPredicate::ULT, iv, len, "").unwrap();
        self.b.build_conditional_branch(c, bb, done).unwrap();
        self.b.position_at_end(bb);
        if let Some(l) = index {
            self.assign_local(l, Val::borrowed(iv.into()), true)?;
        }
        if let Some(l) = elem {
            let ep = if is_array {
                let at = self.llty(seq.ty);
                unsafe { self.b.build_in_bounds_gep(at, data, &[self.i64_ty().const_zero(), iv], "") }.unwrap()
            } else {
                unsafe { self.b.build_in_bounds_gep(elt, data, &[iv], "") }.unwrap()
            };
            let v = self.b.build_load(elt, ep, "").unwrap();
            self.assign_local(l, Val::borrowed(v), true)?;
        }
        self.loop_body(label, done, cont, body)?;
        self.b.position_at_end(cont);
        let iv = self.b.build_load(self.i64_ty(), ctr, "").unwrap().into_int_value();
        let n = self.b.build_int_add(iv, self.i64_ty().const_int(1, false), "").unwrap();
        self.b.build_store(ctr, n).unwrap();
        self.b.build_unconditional_branch(head).unwrap();
        self.b.position_at_end(done);
        Ok(())
    }

    fn for_string(&mut self, label: Label, index: Option<LocalId>, ch: Option<LocalId>, string: &Expr, body: &Block) -> R<()> {
        let sv = self.expr(string)?;
        let tmp = self.scoped_slot(string.ty, sv);
        let s = self.b.build_load(self.ptr_ty(), tmp, "").unwrap();
        let len = self.call_rt_v("slang_string_len", &[s.into()]).into_int_value();
        let ctr = self.entry_alloca(self.i64_ty().into(), "i", false);
        let width = self.entry_alloca(self.i64_ty().into(), "w", false);
        self.b.build_store(ctr, self.i64_ty().const_zero()).unwrap();
        let head = self.block("fstr.head");
        let bb = self.block("fstr.body");
        let cont = self.block("fstr.next");
        let done = self.block("fstr.end");
        self.br(head);
        self.b.position_at_end(head);
        let iv = self.b.build_load(self.i64_ty(), ctr, "").unwrap().into_int_value();
        let c = self.b.build_int_compare(IntPredicate::SLT, iv, len, "").unwrap();
        self.b.build_conditional_branch(c, bb, done).unwrap();
        self.b.position_at_end(bb);
        let r = self.call_rt_v("slang_string_decode_rune", &[s.into(), iv.into(), width.into()]);
        if let Some(l) = index {
            self.assign_local(l, Val::borrowed(iv.into()), true)?;
        }
        if let Some(l) = ch {
            self.assign_local(l, Val::borrowed(r), true)?;
        }
        self.loop_body(label, done, cont, body)?;
        self.b.position_at_end(cont);
        let iv = self.b.build_load(self.i64_ty(), ctr, "").unwrap().into_int_value();
        let w = self.b.build_load(self.i64_ty(), width, "").unwrap().into_int_value();
        let n = self.b.build_int_add(iv, w, "").unwrap();
        self.b.build_store(ctr, n).unwrap();
        self.b.build_unconditional_branch(head).unwrap();
        self.b.position_at_end(done);
        Ok(())
    }

    fn for_map(&mut self, label: Label, key: Option<LocalId>, value: Option<LocalId>, map: &Expr, body: &Block) -> R<()> {
        let (kt, vt) = self.map_kv(map.ty);
        let mv = self.expr(map)?;
        let tmp = self.scoped_slot(map.ty, mv);
        let m = self.b.build_load(self.ptr_ty(), tmp, "").unwrap();
        let it = self.entry_alloca(self.i64_ty().array_type(4).into(), "it", false);
        self.call_rt("slang_map_iter_init", &[it.into(), m.into()]);
        let kp = self.entry_alloca(self.ptr_ty().into(), "kp", false);
        let vp = self.entry_alloca(self.ptr_ty().into(), "vp", false);
        let head = self.block("fm.head");
        let bb = self.block("fm.body");
        let done = self.block("fm.end");
        self.br(head);
        self.b.position_at_end(head);
        let ok = self.call_rt_v("slang_map_iter_next", &[it.into(), kp.into(), vp.into()]).into_int_value();
        self.b.build_conditional_branch(ok, bb, done).unwrap();
        self.b.position_at_end(bb);
        if let Some(l) = key {
            let p = self.b.build_load(self.ptr_ty(), kp, "").unwrap().into_pointer_value();
            let lt = self.llty(kt);
            let k = self.b.build_load(lt, p, "").unwrap();
            self.assign_local(l, Val::borrowed(k), true)?;
        }
        if let Some(l) = value {
            let p = self.b.build_load(self.ptr_ty(), vp, "").unwrap().into_pointer_value();
            let lt = self.llty(vt);
            let v = self.b.build_load(lt, p, "").unwrap();
            self.assign_local(l, Val::borrowed(v), true)?;
        }
        self.loop_body(label, done, head, body)?;
        self.b.position_at_end(done);
        Ok(())
    }

    fn switch(&mut self, sw: &Switch) -> R<()> {
        let subj = match &sw.subject {
            Some((l, e)) => {
                let v = self.expr(e)?;
                self.assign_local(*l, v, true)?;
                Some((*l, e.ty))
            }
            None => None,
        };
        let end = self.block("sw.end");
        self.fs_mut().loops.push(LoopTarget { label: sw.label, brk: end, cont: None });
        let r = (|| -> R<()> {
            for case in &sw.cases {
                let body = self.block("case.body");
                for p in &case.patterns {
                    let next = self.block("case.next");
                    self.pattern(p, subj, body, next)?;
                    self.b.position_at_end(next);
                }
                // No pattern matched: `next` continues with the following case.
                let after = self.b.get_insert_block().unwrap();
                self.b.position_at_end(body);
                self.block_stmts(&case.body)?;
                self.br(end);
                self.b.position_at_end(after);
            }
            if let Some(d) = &sw.default {
                self.block_stmts(d)?;
            }
            self.br(end);
            Ok(())
        })();
        self.fs_mut().loops.pop();
        r?;
        self.b.position_at_end(end);
        Ok(())
    }

    /// Tests pattern `p`; on a match binds and branches to `body`, else to
    /// `next`.
    fn pattern(&mut self, p: &Pattern, subj: Option<(LocalId, TypeId)>, body: BasicBlock<'ctx>, next: BasicBlock<'ctx>) -> R<()> {
        match p {
            Pattern::Cond(e) => {
                let c = self.expr(e)?.get().into_int_value();
                self.b.build_conditional_branch(c, body, next).unwrap();
            }
            Pattern::Variant { variant, bindings } => {
                let Some((l, st)) = subj else { return unsupported("variant pattern without subject") };
                let Type::Enum(d) = self.ty(st).clone() else { return unsupported("variant pattern on a non-enum") };
                let sp = self.local_addr(l);
                let plain = self.prog.enums[d.0 as usize].is_plain;
                let tag = self.b.build_load(self.ctx.i32_type(), sp, "tag").unwrap().into_int_value();
                let c = self.b.build_int_compare(IntPredicate::EQ, tag, self.ctx.i32_type().const_int(*variant as u64, false), "").unwrap();
                let bind = self.block("case.bind");
                self.b.build_conditional_branch(c, bind, next).unwrap();
                self.b.position_at_end(bind);
                if !plain {
                    let et = self.enum_llty(d.0);
                    let vt = self.variant_llty(d.0, *variant);
                    let pp = self.b.build_struct_gep(et, sp, 1, "").unwrap();
                    let fts: Vec<TypeId> = self.prog.enums[d.0 as usize].variants[*variant as usize].fields.iter().map(|f| f.1).collect();
                    for (i, bl) in bindings.iter().enumerate() {
                        if let Some(bl) = bl {
                            let fp = self.b.build_struct_gep(vt, pp, i as u32, "").unwrap();
                            let ft = self.llty(fts[i]);
                            let v = self.b.build_load(ft, fp, "").unwrap();
                            self.assign_local(*bl, Val::borrowed(v), true)?;
                        }
                    }
                }
                self.b.build_unconditional_branch(body).unwrap();
            }
            Pattern::Type { ty, binding } => {
                let Some((l, st)) = subj else { return unsupported("type pattern without subject") };
                let sp = self.local_addr(l);
                let lt = self.llty(st);
                let sv = self.b.build_load(lt, sp, "").unwrap();
                let (m, dp) = self.type_test(st, sv, *ty)?;
                let bind = self.block("case.bind");
                self.b.build_conditional_branch(m, bind, next).unwrap();
                self.b.position_at_end(bind);
                if let Some(bl) = binding {
                    let bt = self.llty(*ty);
                    let v = self.b.build_load(bt, dp, "").unwrap();
                    self.assign_local(*bl, Val::borrowed(v), true)?;
                }
                self.b.build_unconditional_branch(body).unwrap();
            }
        }
        Ok(())
    }

    // ------------------------------------------------------------------
    // return and errors
    // ------------------------------------------------------------------

    fn ret_info(&self) -> R<(PointerValue<'ctx>, TypeId)> {
        match self.fs().ret_slot {
            Some(r) => Ok(r),
            None => unsupported("returning a value from a void function"),
        }
    }

    /// Stores error `err` (owned) as the function's failure result and
    /// jumps to the exit.
    fn return_error(&mut self, err: BasicValueEnum<'ctx>) -> R<()> {
        let (p, t) = self.ret_info()?;
        let Type::Fallible(inner) = self.ty(t).clone() else { return unsupported("error return from a non-fallible function") };
        if *self.ty(inner) == Type::Void {
            self.b.build_store(p, err).unwrap();
        } else {
            let st = self.llty(t).into_struct_type();
            let s = self.b.build_insert_value(st.const_zero(), err, 1, "").unwrap();
            self.b.build_store(p, s).unwrap();
        }
        let exit = self.fs().exit.unwrap();
        self.jump(exit);
        Ok(())
    }

    pub(crate) fn emit_return(&mut self, rv: &ReturnValue) -> R<()> {
        let exit = self.fs().exit.unwrap();
        match rv {
            ReturnValue::Void => {
                if let Some((p, t)) = self.fs().ret_slot {
                    let z = self.zero(t);
                    self.b.build_store(p, z).unwrap();
                }
            }
            ReturnValue::Value(e) => {
                let (p, t) = self.ret_info()?;
                let v = self.expr_owned(e)?;
                if let Type::Fallible(inner) = self.ty(t).clone() {
                    if *self.ty(inner) == Type::Void {
                        self.b.build_store(p, self.ptr_ty().const_null()).unwrap();
                    } else {
                        let st = self.llty(t).into_struct_type();
                        let s = self.b.build_insert_value(st.const_zero(), v, 0, "").unwrap();
                        self.b.build_store(p, s).unwrap();
                    }
                } else {
                    self.b.build_store(p, v).unwrap();
                }
            }
            ReturnValue::Error(e) => {
                let v = self.expr_owned(e)?;
                return self.return_error(v);
            }
        }
        self.jump(exit);
        Ok(())
    }

    /// Splits an owned `T!` into (value or None, error).
    fn split_fallible(&mut self, t: TypeId, v: BasicValueEnum<'ctx>) -> (Option<BasicValueEnum<'ctx>>, PointerValue<'ctx>) {
        let Type::Fallible(inner) = self.ty(t).clone() else { return (Some(v), self.ptr_ty().const_null()) };
        if *self.ty(inner) == Type::Void {
            (None, v.into_pointer_value())
        } else {
            let s = v.into_struct_value();
            (Some(self.b.build_extract_value(s, 0, "").unwrap()), self.b.build_extract_value(s, 1, "err").unwrap().into_pointer_value())
        }
    }

    /// `value?` / `value?(ctx)`.
    pub(crate) fn propagate(&mut self, value: &Expr, ctx: Option<&Expr>) -> R<Val<'ctx>> {
        let fv = self.expr_owned(value)?;
        let (v, err) = self.split_fallible(value.ty, fv);
        let fail = self.block("prop.fail");
        let ok = self.block("prop.ok");
        let isnull = self.b.build_is_null(err, "").unwrap();
        self.b.build_conditional_branch(isnull, ok, fail).unwrap();
        self.b.position_at_end(fail);
        let mut e: BasicValueEnum = err.into();
        if let Some(c) = ctx {
            let cv = self.expr(c)?;
            let w = self.call_rt_v("slang_error_wrap", &[e.into(), cv.get().into()]);
            self.call_rt("slang_release", &[e.into()]);
            self.drop_val(c.ty, cv);
            e = w;
        }
        self.return_error(e)?;
        self.b.position_at_end(ok);
        Ok(match v {
            Some(v) => Val::owned(v),
            None => Val::void(),
        })
    }

    pub(crate) fn try_catch(&mut self, ty: TypeId, value: &Expr, err: Option<LocalId>, handler: &Block, result_used: bool) -> R<Val<'ctx>> {
        let fv = self.expr_owned(value)?;
        let (v, e) = self.split_fallible(value.ty, fv);
        let res = match (result_used, v) {
            (true, Some(v)) => {
                let p = self.entry_alloca(v.get_type(), "try", false);
                self.b.build_store(p, v.get_type().const_zero()).unwrap();
                Some(p)
            }
            _ => None,
        };
        let fail = self.block("catch");
        let ok = self.block("try.ok");
        let cont = self.block("try.end");
        let isnull = self.b.build_is_null(e, "").unwrap();
        self.b.build_conditional_branch(isnull, ok, fail).unwrap();
        self.b.position_at_end(fail);
        match err {
            Some(l) => self.assign_local(l, Val::owned(e.into()), true)?,
            None => {
                self.call_rt("slang_release", &[e.into()]);
            }
        }
        self.block_stmts(handler)?;
        self.br(cont);
        self.b.position_at_end(ok);
        let inner = match self.ty(value.ty) {
            Type::Fallible(t) => *t,
            _ => value.ty,
        };
        if let Some(v) = v {
            match res {
                Some(p) => {
                    self.b.build_store(p, v).unwrap();
                }
                None => self.release_val(inner, v),
            }
        }
        self.br(cont);
        self.b.position_at_end(cont);
        let _ = ty;
        Ok(match res {
            Some(p) => {
                let lt = self.llty(inner);
                Val::owned(self.b.build_load(lt, p, "").unwrap())
            }
            None => Val::void(),
        })
    }

    pub(crate) fn try_else(&mut self, ty: TypeId, value: &Expr, fallback: &TryFallback) -> R<Val<'ctx>> {
        let fv = self.expr_owned(value)?;
        let (v, e) = self.split_fallible(value.ty, fv);
        let res = self.llty_opt(ty).map(|t| self.entry_alloca(t, "tryelse", false));
        let fail = self.block("else");
        let ok = self.block("try.ok");
        let cont = self.block("try.end");
        let isnull = self.b.build_is_null(e, "").unwrap();
        self.b.build_conditional_branch(isnull, ok, fail).unwrap();
        self.b.position_at_end(fail);
        self.call_rt("slang_release", &[e.into()]);
        match fallback {
            TryFallback::Value(x) => {
                let xv = self.expr(x)?;
                match (res, xv.v) {
                    (Some(p), Some(_)) => {
                        let o = self.to_owned(ty, xv);
                        self.b.build_store(p, o).unwrap();
                    }
                    _ => self.drop_val(x.ty, xv),
                }
                self.br(cont);
            }
            TryFallback::Break(l) => {
                let t = self.target(*l)?;
                self.jump(t.brk);
            }
            TryFallback::Continue(l) => {
                let t = self.target(*l)?;
                let Some(c) = t.cont else { return unsupported("continue to a switch") };
                self.jump(c);
            }
            TryFallback::Return(rv) => self.emit_return(rv)?,
        }
        self.b.position_at_end(ok);
        if let (Some(p), Some(v)) = (res, v) {
            self.b.build_store(p, v).unwrap();
        }
        self.br(cont);
        self.b.position_at_end(cont);
        Ok(match res {
            Some(p) => {
                let lt = self.llty(ty);
                Val::owned(self.b.build_load(lt, p, "").unwrap())
            }
            None => Val::void(),
        })
    }
}
