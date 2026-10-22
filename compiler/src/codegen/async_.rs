//! `async` functions as LLVM switched-resume coroutines (ABI.md §10.3,
//! modelled on `runtime/tests/coro_lowering.ll`).
//!
//! Ramp layout: `entry` (allocas) -> `init` (task creation, `coro.id`,
//! `coro.begin`, then zeroing of locals) -> body. `return` goes through the
//! common exit block (defers, release of locals), which completes the task,
//! drops the frame's task reference and branches to `cleanup` (frees the
//! frame; there is no final suspend) and `suspended` (`coro.end`, `ret
//! task`). `await t` registers `zell_coro_resume(hdl)` as a continuation of
//! `t` and suspends unless `t` is already done. Outside a coroutine (C
//! `main`), `await` blocks with `zell_task_await`.

use super::{unsupported, CodeGen, Coro, Val, R};
use crate::hir::{self, Expr, ExprKind};
use crate::types::{Type, TypeId};
use inkwell::llvm_sys::core as llc;
use inkwell::llvm_sys::prelude::{LLVMTypeRef, LLVMValueRef};
use inkwell::types::AsTypeRef;
use inkwell::values::{AsValueRef, BasicValueEnum, IntValue, PointerValue};
use inkwell::IntPredicate;

impl<'a, 'ctx> CodeGen<'a, 'ctx> {
    /// Calls intrinsic `name` (non-overloaded) with raw arguments.
    fn intrinsic(&mut self, name: &str, args: &mut [LLVMValueRef]) -> LLVMValueRef {
        unsafe {
            // `llvm.coro.size.i64` is overloaded on its result type.
            let (base, mut tys): (&str, Vec<LLVMTypeRef>) = match name.strip_suffix(".i64") {
                Some(b) => (b, vec![self.i64_ty().as_type_ref()]),
                None => (name, vec![]),
            };
            let id = llc::LLVMLookupIntrinsicID(base.as_ptr() as *const _, base.len());
            assert!(id != 0, "codegen: unknown intrinsic {name}");
            let f = llc::LLVMGetIntrinsicDeclaration(self.module.as_mut_ptr(), id, tys.as_mut_ptr(), tys.len());
            let fty = llc::LLVMGlobalGetValueType(f);
            llc::LLVMBuildCall2(self.b.as_mut_ptr(), fty, f, args.as_mut_ptr(), args.len() as u32, c"".as_ptr())
        }
    }

    fn token_none(&self) -> LLVMValueRef {
        unsafe {
            let t: LLVMTypeRef = llc::LLVMTokenTypeInContext(self.ctx.raw());
            llc::LLVMConstNull(t)
        }
    }

    /// Emits the ramp prologue of async function `f` into the init block.
    pub(crate) fn coro_prologue(&mut self, f: &hir::Function) -> R<()> {
        let body = self.b.get_insert_block().unwrap();
        let init = self.fs().init;
        self.b.position_at_end(init);
        let size = self.i64_ty().const_int(self.size_of(f.ret), false);
        let ops = self.value_ops(f.ret);
        let task = self.call_rt_v("zell_task_new", &[size.into(), ops.into()]).into_pointer_value();
        self.call_rt("zell_retain", &[task.into()]);
        let null = self.ptr_ty().const_null().as_value_ref();
        let zero = self.ctx.i32_type().const_zero().as_value_ref();
        let id = self.intrinsic("llvm.coro.id", &mut [zero, null, null, null]);
        let sz = self.intrinsic("llvm.coro.size.i64", &mut []);
        let szv = unsafe { IntValue::new(sz) };
        let mem = self.call_rt_v("zell_coro_frame_alloc", &[szv.into()]);
        let hdl = self.intrinsic("llvm.coro.begin", &mut [id, mem.as_value_ref()]);
        let hdl = unsafe { PointerValue::new(hdl) };
        // Zeroing stores emitted later by `entry_alloca` follow this code.
        let cleanup = self.block("coro.cleanup");
        let suspended = self.block("coro.suspended");
        self.b.position_at_end(cleanup);
        let fr = self.intrinsic("llvm.coro.free", &mut [id, hdl.as_value_ref()]);
        let frv = unsafe { PointerValue::new(fr) };
        self.call_rt("zell_coro_frame_free", &[frv.into()]);
        self.b.build_unconditional_branch(suspended).unwrap();
        self.b.position_at_end(suspended);
        let fls = self.ctx.bool_type().const_zero().as_value_ref();
        let none = self.token_none();
        self.intrinsic("llvm.coro.end", &mut [hdl.as_value_ref(), fls, none]);
        self.b.build_return(Some(&task)).unwrap();
        self.fs_mut().coro = Some(Coro { hdl, task, cleanup, suspended });
        self.b.position_at_end(body);
        Ok(())
    }

    /// Completes the task with the result slot and leaves the coroutine.
    pub(crate) fn coro_return(&mut self) -> R<()> {
        let co = self.fs().coro.unwrap();
        let (slot, rt) = match self.fs().ret_slot {
            Some((p, t)) => (p, Some(t)),
            None => (self.ptr_ty().const_null(), None),
        };
        self.call_rt("zell_task_complete", &[co.task.into(), slot.into()]);
        if let Some(t) = rt {
            if self.needs_rc(t) {
                let lt = self.llty(t);
                let v = self.b.build_load(lt, slot, "").unwrap();
                self.release_val(t, v);
            }
        }
        self.call_rt("zell_release", &[co.task.into()]);
        self.b.build_unconditional_branch(co.cleanup).unwrap();
        Ok(())
    }

    /// Waits for task `t`; returns its (borrowed) result slot.
    fn await_slot(&mut self, t: PointerValue<'ctx>) -> PointerValue<'ctx> {
        let Some(co) = self.fs().coro else {
            return self.call_rt_v("zell_task_await", &[t.into()]).into_pointer_value();
        };
        let ready = self.block("await.ready");
        let try_ = self.block("await.try");
        let susp = self.block("await.suspend");
        let done = self.call_rt_v("zell_task_is_done", &[t.into()]).into_int_value();
        self.b.build_conditional_branch(done, ready, try_).unwrap();
        self.b.position_at_end(try_);
        self.dbga_suspend();
        let save = self.intrinsic("llvm.coro.save", &mut [co.hdl.as_value_ref()]);
        let resume = self.rt("zell_coro_resume").as_global_value().as_pointer_value();
        let reg = self.call_rt_v("zell_task_add_continuation", &[t.into(), resume.into(), co.hdl.into()]).into_int_value();
        self.b.build_conditional_branch(reg, susp, ready).unwrap();
        self.b.position_at_end(susp);
        let fls = self.ctx.bool_type().const_zero().as_value_ref();
        let s = self.intrinsic("llvm.coro.suspend", &mut [save, fls]);
        let sv = unsafe { IntValue::new(s) };
        let i8t = self.ctx.i8_type();
        self.b.build_switch(sv, co.suspended, &[(i8t.const_zero(), ready), (i8t.const_int(1, false), co.cleanup)]).unwrap();
        self.b.position_at_end(ready);
        self.dbga_resume();
        self.call_rt_v("zell_task_result", &[t.into()]).into_pointer_value()
    }

    /// Loads an owned copy of a `T` result from `slot`.
    fn load_result(&mut self, t: TypeId, slot: PointerValue<'ctx>) -> Option<BasicValueEnum<'ctx>> {
        let lt = self.llty_opt(t)?;
        let v = self.b.build_load(lt, slot, "").unwrap();
        self.retain_val(t, v);
        Some(v)
    }

    /// `await t` with result type `ty`.
    pub(crate) fn await_expr(&mut self, ty: TypeId, t: &Expr) -> R<Val<'ctx>> {
        if let ExprKind::TaskAll(tasks) = &t.kind {
            return self.await_all(ty, tasks);
        }
        if let ExprKind::Builtin { op: crate::hir::Builtin::TaskAny, args } = &t.kind {
            return self.await_any(ty, args);
        }
        let tv = self.expr(t)?;
        let tp = tv.get().into_pointer_value();
        let slot = self.await_slot(tp);
        let r = self.load_result(ty, slot);
        self.drop_val(t.ty, tv);
        Ok(match r {
            Some(v) => Val::owned(v),
            None => Val::void(),
        })
    }

    /// `await task.Any(t1, ..., tn)`: `zell_task_any` yields the index of
    /// the first task to complete; its result is copied out (retained).
    fn await_any(&mut self, ty: TypeId, tasks: &[Expr]) -> R<Val<'ctx>> {
        let n = tasks.len() as u32;
        let arr_ty = self.ptr_ty().array_type(n);
        let arr = self.entry_alloca(arr_ty.into(), "any.tasks", false);
        let mut tvs = Vec::new();
        let d = self.depth();
        for (i, t) in tasks.iter().enumerate() {
            let v = self.expr(t)?;
            self.hold(t.ty, v);
            let p = unsafe { self.b.build_in_bounds_gep(arr_ty, arr, &[self.i64_ty().const_zero(), self.i64_ty().const_int(i as u64, false)], "") }.unwrap();
            self.b.build_store(p, v.get()).unwrap();
            tvs.push(v);
        }
        self.forget_from(d);
        let any = self.call_rt_v("zell_task_any", &[arr.into(), self.i64_ty().const_int(n as u64, false).into()]).into_pointer_value();
        let slot = self.await_slot(any);
        let idx = self.b.build_load(self.i64_ty(), slot, "any.idx").unwrap().into_int_value();
        self.call_rt("zell_release", &[any.into()]);
        let p = unsafe { self.b.build_in_bounds_gep(arr_ty, arr, &[self.i64_ty().const_zero(), idx], "") }.unwrap();
        let winner = self.b.build_load(self.ptr_ty(), p, "").unwrap().into_pointer_value();
        let rslot = self.call_rt_v("zell_task_result", &[winner.into()]).into_pointer_value();
        let r = self.load_result(ty, rslot);
        for (t, tv) in tasks.iter().zip(tvs) {
            self.drop_val(t.ty, tv);
        }
        Ok(match r {
            Some(v) => Val::owned(v),
            None => Val::void(),
        })
    }

    /// `await task.All(t1, ..., tn)`: awaits each task in order and builds
    /// the tuple (or `(..)!`, failing with the first error in order).
    fn await_all(&mut self, ty: TypeId, tasks: &[Expr]) -> R<Val<'ctx>> {
        let mut tvs = Vec::new();
        let d = self.depth();
        for t in tasks {
            let v = self.expr(t)?;
            self.hold(t.ty, v);
            tvs.push(v);
        }
        self.forget_from(d);
        let mut results = Vec::new();
        for (t, tv) in tasks.iter().zip(&tvs) {
            let Type::Task(rt) = self.ty(t.ty).clone() else { return unsupported("task.All over a non-task") };
            let slot = self.await_slot(tv.get().into_pointer_value());
            results.push((rt, self.load_result(rt, slot)));
        }
        for (t, tv) in tasks.iter().zip(tvs) {
            self.drop_val(t.ty, tv);
        }
        let (tuple_ty, fallible) = match self.ty(ty) {
            Type::Fallible(inner) => (*inner, true),
            _ => (ty, false),
        };
        let st = self.llty(tuple_ty).into_struct_type();
        let mut agg: BasicValueEnum = st.const_zero().into();
        let mut err = self.ptr_ty().const_null();
        let mut idx = 0u32;
        for (rt, v) in results {
            let Some(v) = v else { continue };
            let (val, e) = match self.ty(rt) {
                Type::Fallible(inner) if fallible => {
                    if *self.ty(*inner) == Type::Void {
                        (None, Some(v.into_pointer_value()))
                    } else {
                        let s = v.into_struct_value();
                        (Some(self.b.build_extract_value(s, 0, "").unwrap()), Some(self.b.build_extract_value(s, 1, "").unwrap().into_pointer_value()))
                    }
                }
                _ => (Some(v), None),
            };
            if let Some(e) = e {
                let first = self.b.build_is_null(err, "").unwrap();
                let keep = self.b.build_select(first, e, err, "").unwrap().into_pointer_value();
                let drop = self.b.build_select(first, self.ptr_ty().const_null(), e, "").unwrap();
                self.call_rt("zell_release", &[drop.into()]);
                err = keep;
            }
            if let Some(val) = val {
                if idx < st.count_fields() {
                    agg = self.b.build_insert_value(agg.into_struct_value(), val, idx, "").unwrap().into_struct_value().into();
                }
                idx += 1;
            }
        }
        if !fallible {
            return Ok(Val::owned(agg));
        }
        // On failure drop the partial values and return {zero, err}.
        let failed = self.b.build_is_not_null(err, "").unwrap();
        let rel = self.block("all.fail");
        let join = self.block("all.join");
        self.b.build_conditional_branch(failed, rel, join).unwrap();
        self.b.position_at_end(rel);
        self.release_val(tuple_ty, agg);
        self.b.build_unconditional_branch(join).unwrap();
        self.b.position_at_end(join);
        let z = st.const_zero();
        let tuple = self.b.build_select(failed, BasicValueEnum::from(z), agg, "").unwrap();
        let ft = self.llty(ty).into_struct_type();
        let s = self.b.build_insert_value(ft.get_undef(), tuple, 0, "").unwrap();
        let s = self.b.build_insert_value(s, err, 1, "").unwrap();
        let _ = IntPredicate::EQ;
        Ok(Val::owned(s.into_struct_value().into()))
    }
}
