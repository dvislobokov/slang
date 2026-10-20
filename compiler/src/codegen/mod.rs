//! LLVM code generation: lowers a typed [`hir::Program`] to an LLVM module
//! that links against the C runtime (`runtime/libslang_rt.a`, contract in
//! `runtime/ABI.md`).
//!
//! # Value representation (compiler-side ABI, v0.1)
//!
//! | Slang type | LLVM | Notes |
//! |---|---|---|
//! | `int`, `uint`, `intN`, `uintN`, `byte` | `i64`, `iN` | signedness only in instructions |
//! | `float` / `float32` | `double` / `float` | |
//! | `bool` | `i1` | `zeroext` at every C call boundary; 1 byte in memory |
//! | `char` | `i32` | code point |
//! | `string` | `ptr` | runtime `slang_string*`, NULL is `""`; literals are immortal constant globals |
//! | `*T` | `ptr` | address of the `T` itself (the **payload**, i.e. heap object + 16 for `new`) |
//! | struct | named LLVM struct by value | fields in HIR order |
//! | `[N]T` | `[N x T]` | by value |
//! | `[]T` | `{ptr data, i64 len, i64 cap, ptr backing}` | ABI.md §5, the slice value owns one reference to `backing` |
//! | `map[K]V` | `ptr` | runtime `slang_map*`; `string`/`int` keys use the fast paths |
//! | `error` | `ptr` | runtime `slang_error*`, NULL = no error |
//! | `T!` | `{T, ptr err}` | `void!` is just the `ptr err` |
//! | tuple | anonymous LLVM struct | |
//! | plain enum | `i32` tag | variant index |
//! | data enum | `{i32 tag, [W x i64] payload}` | payload is the variant's fields as an anonymous struct, 8-aligned |
//! | interface | `{ptr data, ptr vtable}` | vtable `{ptr typeid, i64 boxed, ptr method0, ...}` (methods in interface order) |
//! | `any` | `{ptr data, ptr typeinfo}` | always boxed |
//! | `func(..)` | `{ptr fn, ptr env}` | `fn(env, args...)`; `env` is NULL or an env object |
//! | `Task<T>` | `ptr` | runtime `slang_task*` |
//!
//! **Heap objects.** Every heap allocation made by compiled code is an ABI.md
//! §3 object (`slang_object_new`): `new T`, boxes (struct values stored in
//! interfaces / `any` / errors), *cells* and closure environments. One
//! `slang_type_info` per boxed type `T` (`ti.<T>`) has a `drop` that releases
//! the references inside the `T` payload at offset 16. Its address is also
//! the run-time type id used by type switches and `errors.As`; the type id of
//! a stored `*S` is `ti.<*S>`.
//!
//! **Interfaces.** Storing a struct value boxes a copy (`boxed = 1`, the box
//! is refcounted through the interface value); storing a `*S` stores the
//! pointer (`boxed = 0`). Method `m` is `vtable[2 + m](data, args...)`;
//! methods promoted from embedded structs get an adjusting thunk. Storing a
//! struct in an `error` boxes it and calls `slang_error_new_typed` with a
//! `Message()` thunk; `error` method 0 is `slang_error_message`.
//!
//! **Closures.** A closure is `{fn, env}`. `env` is NULL for closures without
//! captures, else an object `{hdr, i64 n, ptr cell[n]}` whose drop releases
//! the cells. Captured locals (`Local::captured`) live in *cells*: objects
//! `{hdr, T value}`; the function's slot holds the cell pointer, every
//! execution of the `Let` creates a fresh cell, and closures retain the
//! cells, so variables are shared by reference. Named functions used as
//! values get a thunk `fn(env, args) -> f(args)`. Lifted lambdas take the
//! env pointer as an extra first parameter.
//!
//! **Address-taken locals** (`Local::address_taken`) also live in cells
//! (no escape analysis yet); such cells are never released (see below).
//!
//! **Fallible results** travel as first-class aggregates between Slang
//! functions. Stdlib externs follow `slang_rt.h`: `[]T` params as
//! `const slang_slice*`, a `[]T` result through a trailing out-pointer,
//! `T!` as `T` plus a trailing `slang_error **err` (`void!`: only the
//! out-error).
//!
//! **Functions.** Every function has a single exit block: `return` stores
//! the result into a slot and branches there; the exit block runs deferred
//! closures (LIFO, up to 64 per call, kept in an alloca'd array), releases
//! the locals and returns. `async` functions are LLVM switched-resume
//! coroutines following ABI.md §10.3 (see [`async_`]). The build pipeline
//! (`crate::build`) runs `default<On>`, which in LLVM 18 includes the
//! coroutine passes (coro-early, coro-split, coro-elide, coro-cleanup; at
//! O0 via the CoroConditionalWrapper), so coroutines are lowered at every
//! optimisation level.
//!
//! # Reference counting (v0.1)
//!
//! Managed: `string`, `[]T`, `map`, `error`, `Task`, closures (their env),
//! interface/`any` boxes, and aggregates (structs, tuples, arrays, `T!`,
//! data enums) containing those, via generated per-type retain/release
//! helpers (also used as `slang_value_ops` for containers).
//!
//! * Expression results are *owned* (+1: calls, constructors, concatenation)
//!   or *borrowed* (loads from places, literals). Storing a borrowed value
//!   into a location retains it; owned temporaries are released right
//!   after their use.
//! * Stores release the previous value (after retaining the new one).
//! * Parameters are retained on entry (callee owns its copy); every local
//!   (and every loop-sequence temporary) is released once, at function exit
//!   (its slot is zeroed at entry, so paths that never initialised it are
//!   fine). Re-executing a `Let` (loops) releases the old value.
//! * Results are returned +1; runtime borrowed results are retained.
//!
//! Not released (leaks, by design for v0.1): `*T` pointers are *unmanaged*
//! (a pointer may point into the middle of an object), so objects from `new`
//! and cells of address-taken locals are never freed; owned temporaries
//! that are live when an expression exits abnormally (`?`, `try ... else
//! return`, `break` from inside an expression) leak; values in the 64-slot
//! defer array beyond a panic. No double frees: every release pairs with an
//! owned reference.

mod async_;
mod expr;
mod funcs;
mod iface;
mod ops;
mod print;
mod rc;
mod runtime;
mod stmt;
mod types;

use crate::hir::{self, FuncId, Program};
use crate::types::TypeId;
use inkwell::basic_block::BasicBlock;
use inkwell::builder::Builder;
use inkwell::context::Context;
use inkwell::module::{Linkage, Module};
use inkwell::targets::{CodeModel, InitializationConfig, RelocMode, Target, TargetData, TargetMachine};
use inkwell::types::StructType;
use inkwell::values::{BasicValueEnum, FunctionValue, GlobalValue, PointerValue};
use inkwell::OptimizationLevel;
use std::collections::HashMap;

/// Codegen result; the error is a diagnostic message.
pub(crate) type R<T> = Result<T, String>;

/// "not supported in codegen yet: X".
pub(crate) fn unsupported<T>(what: impl std::fmt::Display) -> R<T> {
    Err(format!("not supported in codegen yet: {what}"))
}

/// A value produced by expression codegen, with its ownership.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Val<'ctx> {
    /// `None` for `void` expressions.
    pub v: Option<BasicValueEnum<'ctx>>,
    /// True if the value carries a +1 reference the consumer must
    /// release or move.
    pub owned: bool,
}

impl<'ctx> Val<'ctx> {
    pub fn owned(v: BasicValueEnum<'ctx>) -> Self {
        Val { v: Some(v), owned: true }
    }
    pub fn borrowed(v: BasicValueEnum<'ctx>) -> Self {
        Val { v: Some(v), owned: false }
    }
    pub fn void() -> Self {
        Val { v: None, owned: false }
    }
    pub fn get(&self) -> BasicValueEnum<'ctx> {
        self.v.expect("codegen: void value used")
    }
}

/// Storage of a local variable.
#[derive(Clone, Copy, Debug)]
pub(crate) struct LocalSlot<'ctx> {
    /// The alloca: holds the value, or (for cells) the cell pointer.
    pub ptr: PointerValue<'ctx>,
    pub cell: bool,
}

/// A `break`/`continue` target.
#[derive(Clone, Copy, Debug)]
pub(crate) struct LoopTarget<'ctx> {
    pub label: hir::Label,
    pub brk: BasicBlock<'ctx>,
    pub cont: Option<BasicBlock<'ctx>>,
}

/// Coroutine state of an `async` function being emitted.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Coro<'ctx> {
    pub hdl: PointerValue<'ctx>,
    pub task: PointerValue<'ctx>,
    pub cleanup: BasicBlock<'ctx>,
    pub suspended: BasicBlock<'ctx>,
}

/// Per-function emission state.
pub(crate) struct FnState<'a, 'ctx> {
    pub func: FunctionValue<'ctx>,
    /// The HIR function, if any (`None` for C `main` and helpers).
    pub hir: Option<&'a hir::Function>,
    pub init: BasicBlock<'ctx>,
    pub alloca_b: Builder<'ctx>,
    pub init_b: Builder<'ctx>,
    pub locals: Vec<Option<LocalSlot<'ctx>>>,
    pub env: Option<PointerValue<'ctx>>,
    /// Result slot and its (representation) type.
    pub ret_slot: Option<(PointerValue<'ctx>, TypeId)>,
    pub exit: Option<BasicBlock<'ctx>>,
    pub loops: Vec<LoopTarget<'ctx>>,
    /// Defer array (`[64 x {ptr, ptr}]`) and count.
    pub defers: Option<(PointerValue<'ctx>, PointerValue<'ctx>)>,
    /// Zero-initialised slots released at exit (loop sequences).
    pub scoped: Vec<(PointerValue<'ctx>, TypeId)>,
    pub coro: Option<Coro<'ctx>>,
}

/// The code generator.
pub struct CodeGen<'a, 'ctx> {
    pub(crate) ctx: &'ctx Context,
    pub(crate) module: Module<'ctx>,
    pub(crate) b: Builder<'ctx>,
    pub(crate) prog: &'a Program,
    pub(crate) td: TargetData,
    pub(crate) slice_struct: StructType<'ctx>,
    pub(crate) struct_tys: HashMap<u32, StructType<'ctx>>,
    pub(crate) enum_tys: HashMap<u32, StructType<'ctx>>,
    pub(crate) funcs: Vec<FunctionValue<'ctx>>,
    pub(crate) globals: Vec<GlobalValue<'ctx>>,
    pub(crate) strings: HashMap<String, PointerValue<'ctx>>,
    pub(crate) cstrings: HashMap<String, PointerValue<'ctx>>,
    pub(crate) type_infos: HashMap<TypeId, GlobalValue<'ctx>>,
    pub(crate) rc_helpers: HashMap<TypeId, (FunctionValue<'ctx>, FunctionValue<'ctx>)>,
    pub(crate) value_ops: HashMap<TypeId, PointerValue<'ctx>>,
    pub(crate) vtables: HashMap<(TypeId, TypeId), GlobalValue<'ctx>>,
    pub(crate) thunks: HashMap<FuncId, FunctionValue<'ctx>>,
    pub(crate) method_thunks: HashMap<(u32, String), FunctionValue<'ctx>>,
    pub(crate) msg_thunks: HashMap<u32, FunctionValue<'ctx>>,
    pub(crate) tostr_fns: HashMap<TypeId, FunctionValue<'ctx>>,
    pub(crate) enum_names: HashMap<u32, GlobalValue<'ctx>>,
    /// Map descriptors by `(key type, value type)` (packed).
    pub(crate) map_descs: HashMap<u64, PointerValue<'ctx>>,
    pub(crate) env_ti: Option<GlobalValue<'ctx>>,
    pub(crate) f: Option<FnState<'a, 'ctx>>,
    pub(crate) counter: u32,
}

/// Creates a target machine for the host.
pub fn host_machine(opt: OptimizationLevel) -> Result<TargetMachine, String> {
    Target::initialize_native(&InitializationConfig::default())?;
    let triple = TargetMachine::get_default_triple();
    let target = Target::from_triple(&triple).map_err(|e| e.to_string())?;
    let cpu = TargetMachine::get_host_cpu_name();
    let features = TargetMachine::get_host_cpu_features();
    target
        .create_target_machine(&triple, cpu.to_str().unwrap_or("generic"), features.to_str().unwrap_or(""), opt, RelocMode::PIC, CodeModel::Default)
        .ok_or_else(|| "cannot create a target machine for the host".to_string())
}

/// Lowers `program` into a new LLVM module named `module_name` for the host
/// target. Returns the diagnostics of unsupported constructs on failure.
pub fn emit<'ctx>(program: &Program, ctx: &'ctx Context, module_name: &str) -> Result<Module<'ctx>, Vec<String>> {
    let machine = host_machine(OptimizationLevel::None).map_err(|e| vec![e])?;
    let module = ctx.create_module(module_name);
    module.set_triple(&machine.get_triple());
    let td = machine.get_target_data();
    module.set_data_layout(&td.get_data_layout());
    let ptr = ctx.ptr_type(inkwell::AddressSpace::default());
    let slice_struct = ctx.opaque_struct_type("slang_slice");
    slice_struct.set_body(&[ptr.into(), ctx.i64_type().into(), ctx.i64_type().into(), ptr.into()], false);
    let mut cg = CodeGen {
        ctx,
        module,
        b: ctx.create_builder(),
        prog: program,
        td,
        slice_struct,
        struct_tys: HashMap::new(),
        enum_tys: HashMap::new(),
        funcs: Vec::new(),
        globals: Vec::new(),
        strings: HashMap::new(),
        cstrings: HashMap::new(),
        type_infos: HashMap::new(),
        rc_helpers: HashMap::new(),
        value_ops: HashMap::new(),
        vtables: HashMap::new(),
        thunks: HashMap::new(),
        method_thunks: HashMap::new(),
        msg_thunks: HashMap::new(),
        tostr_fns: HashMap::new(),
        enum_names: HashMap::new(),
        map_descs: HashMap::new(),
        env_ti: None,
        f: None,
        counter: 0,
    };
    let mut errors = Vec::new();
    if let Err(e) = cg.declare_all() {
        return Err(vec![e]);
    }
    for (i, f) in program.funcs.iter().enumerate() {
        if let Err(e) = cg.emit_function(FuncId(i as u32)) {
            errors.push(format!("{e} (in {})", f.full_name));
            // Leave a valid (trapping) body so the module stays well formed.
            cg.f = None;
            let fv = cg.funcs[i];
            for bb in fv.get_basic_blocks() {
                unsafe {
                    let _ = bb.delete();
                }
            }
        }
    }
    if program.entry.is_some() {
        if let Err(e) = cg.emit_c_main() {
            errors.push(e);
        }
    }
    if errors.is_empty() {
        Ok(cg.module)
    } else {
        Err(errors)
    }
}

impl<'a, 'ctx> CodeGen<'a, 'ctx> {
    /// Declares every function and global.
    fn declare_all(&mut self) -> R<()> {
        for f in &self.prog.funcs {
            let fty = self.fn_llty(f)?;
            let fv = self.module.add_function(&f.symbol, fty, Some(Linkage::Internal));
            if f.is_async {
                fv.add_attribute(inkwell::attributes::AttributeLoc::Function, self.attr("presplitcoroutine"));
            }
            self.funcs.push(fv);
        }
        for g in &self.prog.globals {
            let ty = self.llty(g.ty);
            let gv = self.module.add_global(ty, None, &g.symbol);
            gv.set_linkage(Linkage::Internal);
            gv.set_initializer(&ty.const_zero());
            self.globals.push(gv);
        }
        Ok(())
    }

    /// A fresh name suffix.
    pub(crate) fn fresh(&mut self) -> u32 {
        self.counter += 1;
        self.counter
    }

    pub(crate) fn fs(&self) -> &FnState<'a, 'ctx> {
        self.f.as_ref().expect("codegen: no current function")
    }

    pub(crate) fn fs_mut(&mut self) -> &mut FnState<'a, 'ctx> {
        self.f.as_mut().expect("codegen: no current function")
    }

    /// Starts emitting function `func`: creates the entry (allocas), init
    /// (zeroing) and body blocks and positions the builder in the body.
    pub(crate) fn begin_fn(&mut self, func: FunctionValue<'ctx>, hir: Option<&'a hir::Function>) -> Option<FnState<'a, 'ctx>> {
        let entry = self.ctx.append_basic_block(func, "entry");
        let init = self.ctx.append_basic_block(func, "init");
        let body = self.ctx.append_basic_block(func, "body");
        let alloca_b = self.ctx.create_builder();
        alloca_b.position_at_end(entry);
        let init_b = self.ctx.create_builder();
        init_b.position_at_end(init);
        let st = FnState {
            func,
            hir,
            init,
            alloca_b,
            init_b,
            locals: Vec::new(),
            env: None,
            ret_slot: None,
            exit: None,
            loops: Vec::new(),
            defers: None,
            scoped: Vec::new(),
            coro: None,
        };
        let saved = self.f.replace(st);
        self.b.position_at_end(body);
        saved
    }

    /// Finishes the current function: links entry -> init -> body and
    /// terminates dangling blocks with `unreachable`; restores `saved`.
    pub(crate) fn end_fn(&mut self, saved: Option<FnState<'a, 'ctx>>, saved_pos: Option<BasicBlock<'ctx>>) {
        let st = self.f.take().expect("codegen: end_fn without function");
        // entry -> (prologue chain) is set up by the caller via `init`.
        let blocks = st.func.get_basic_blocks();
        let body = blocks[2];
        st.alloca_b.build_unconditional_branch(st.init).unwrap();
        if st.init.get_terminator().is_none() {
            st.init_b.build_unconditional_branch(body).unwrap();
        }
        for bb in st.func.get_basic_blocks() {
            if bb.get_terminator().is_none() {
                self.b.position_at_end(bb);
                self.b.build_unreachable().unwrap();
            }
        }
        self.f = saved;
        if let Some(bb) = saved_pos {
            self.b.position_at_end(bb);
        }
    }

    /// An alloca in the entry block; zero-initialised (in the init block)
    /// when `zero`.
    pub(crate) fn entry_alloca(&mut self, ty: inkwell::types::BasicTypeEnum<'ctx>, name: &str, zero: bool) -> PointerValue<'ctx> {
        let st = self.fs();
        let p = st.alloca_b.build_alloca(ty, name).unwrap();
        if zero {
            st.init_b.build_store(p, ty.const_zero()).unwrap();
        }
        p
    }

    /// Spills `v` into a fresh entry alloca and returns its address.
    pub(crate) fn spill(&mut self, v: BasicValueEnum<'ctx>) -> PointerValue<'ctx> {
        let p = self.entry_alloca(v.get_type(), "spill", false);
        self.b.build_store(p, v).unwrap();
        p
    }

    /// Continues emission in a new block without predecessors (after a
    /// terminator such as `br`, `ret` or `unreachable`).
    pub(crate) fn dead_block(&mut self) {
        let f = self.fs().func;
        let bb = self.ctx.append_basic_block(f, "dead");
        self.b.position_at_end(bb);
    }

    /// Appends a block to the current function.
    pub(crate) fn block(&mut self, name: &str) -> BasicBlock<'ctx> {
        let f = self.fs().func;
        self.ctx.append_basic_block(f, name)
    }

    /// Branches to `bb` unless the current block is already terminated.
    pub(crate) fn br(&mut self, bb: BasicBlock<'ctx>) {
        if self.b.get_insert_block().and_then(|b| b.get_terminator()).is_none() {
            self.b.build_unconditional_branch(bb).unwrap();
        }
    }

    /// Emits the C `main(argc, argv)` (ABI.md §11).
    fn emit_c_main(&mut self) -> R<()> {
        let i32t = self.ctx.i32_type();
        let fty = i32t.fn_type(&[i32t.into(), self.ptr_ty().into()], false);
        let main = self.module.add_function("main", fty, Some(Linkage::External));
        let saved = self.begin_fn(main, None);
        let argc = main.get_nth_param(0).unwrap();
        let argv = main.get_nth_param(1).unwrap();
        self.call_rt("slang_runtime_init", &[argc.into(), argv.into()]);
        for gid in &self.prog.global_init_order {
            let g = &self.prog.globals[gid.0 as usize];
            let gv = self.globals[gid.0 as usize].as_pointer_value();
            let val = match (&g.init, &g.value) {
                (Some(init), _) => self.expr_owned(init)?,
                (None, Some(c)) => {
                    let e = hir::Expr::new(hir::ExprKind::Const(c.clone()), g.ty, g.span);
                    self.expr_owned(&e)?
                }
                (None, None) => continue,
            };
            self.b.build_store(gv, val).unwrap();
        }
        let entry = self.prog.entry.unwrap();
        let ef = self.funcs[entry.0 as usize];
        let r = self.b.build_call(ef, &[], "").unwrap().try_as_basic_value().left();
        if self.prog.funcs[entry.0 as usize].is_async {
            let t = r.unwrap();
            self.call_rt("slang_task_await", &[t.into()]);
            self.call_rt("slang_release", &[t.into()]);
        }
        self.call_rt("slang_runtime_shutdown", &[]);
        self.b.build_return(Some(&i32t.const_zero())).unwrap();
        self.end_fn(saved, None);
        Ok(())
    }
}
