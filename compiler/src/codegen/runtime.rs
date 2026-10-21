//! Declarations of the C runtime (`runtime/include/slang_rt.h`) and of the
//! stdlib externs named by `sema::stdlib`, plus call helpers that apply the
//! C ABI attributes (`zeroext` on `bool`, `noreturn` on panics).
//!
//! Signatures are written in a tiny notation: `ret:params`, one letter per
//! type: `v` void, `p` ptr, `l` i64, `i` i32, `b` bool (`zeroext i1`),
//! `d` double, `f` float, `c` i8.

use super::CodeGen;
use inkwell::attributes::{Attribute, AttributeLoc};
use inkwell::module::Linkage;
use inkwell::types::{BasicMetadataTypeEnum, BasicType, BasicTypeEnum};
use inkwell::values::{BasicMetadataValueEnum, BasicValueEnum, FunctionValue};

/// Runtime functions: (symbol, signature, noreturn).
const RUNTIME: &[(&str, &str, bool)] = &[
    ("slang_alloc", "p:l", false),
    ("slang_free", "v:p", false),
    ("slang_object_new", "p:pl", false),
    ("slang_cycles_new_entry", "p:pl", false),
    ("slang_cycles_new_dyn", "p:pl", false),
    ("slang_arena_object", "p:lpl", false),
    ("slang_arena_slice", "v:plllp", false),
    ("slang_retain", "v:p", false),
    ("slang_release", "v:p", false),
    ("slang_ptr_retain", "v:p", false),
    ("slang_ptr_release", "v:p", false),
    ("slang_ptr_release_noroot", "v:pl", false),
    ("slang_weak_new", "p:p", false),
    ("slang_weak_get", "p:p", false),
    ("slang_leak_check_enable", "v:i", false),
    ("slang_leak_fields", "v:pp", false),
    ("slang_leak_acyclic", "v:pp", false),
    ("slang_string_concat", "p:pp", false),
    ("slang_string_append", "p:pp", false),
    ("slang_string_compare", "i:pp", false),
    ("slang_string_eq", "b:pp", false),
    ("slang_string_len", "l:p", false),
    ("slang_string_slice", "p:pll", false),
    ("slang_string_decode_rune", "i:plp", false),
    ("slang_string_from_int", "p:l", false),
    ("slang_string_from_uint", "p:l", false),
    ("slang_string_from_float", "p:d", false),
    ("slang_string_from_bool", "p:b", false),
    ("slang_string_from_rune", "p:i", false),
    ("slang_slice_new_ops", "v:plllp", false),
    ("slang_slice_append", "v:plpp", false),
    ("slang_slice_sub", "v:pplll", false),
    ("slang_map_new", "p:pl", false),
    ("slang_map_new_str", "p:lp", false),
    ("slang_map_new_int", "p:lp", false),
    ("slang_map_len", "l:p", false),
    ("slang_map_get", "p:pp", false),
    ("slang_map_set", "v:ppp", false),
    ("slang_map_get_or_insert", "p:pp", false),
    ("slang_map_delete", "b:pp", false),
    ("slang_map_get_str", "p:pp", false),
    ("slang_map_set_str", "v:ppp", false),
    ("slang_map_delete_str", "b:pp", false),
    ("slang_map_get_int", "p:pl", false),
    ("slang_map_set_int", "v:plp", false),
    ("slang_map_delete_int", "b:pl", false),
    ("slang_map_iter_init", "v:pp", false),
    ("slang_map_iter_next", "b:ppp", false),
    ("slang_hash_bytes", "l:pl", false),
    ("slang_error_new", "p:p", false),
    ("slang_error_new_typed", "p:ppp", false),
    ("slang_error_wrap", "p:pp", false),
    ("slang_error_message", "p:p", false),
    ("slang_error_as", "p:pp", false),
    ("slang_error_created", "p:pp", false),
    ("slang_error_returned", "p:ppp", false),
    ("slang_error_report", "v:p", false),
    ("slang_panic", "v:p", true),
    ("slang_panic_str", "v:p", true),
    ("slang_panic_error", "v:p", true),
    ("slang_panic_nil", "v:", true),
    ("slang_panic_bounds", "v:ll", true),
    ("slang_print_str", "v:p", false),
    ("slang_print_int", "v:l", false),
    ("slang_print_uint", "v:l", false),
    ("slang_print_float", "v:d", false),
    ("slang_float32_shortest", "d:f", false),
    ("slang_print_bool", "v:b", false),
    ("slang_print_rune", "v:i", false),
    ("slang_print_space", "v:", false),
    ("slang_print_newline", "v:", false),
    ("slang_coro_resume", "v:p", false),
    ("slang_task_new", "p:lp", false),
    ("slang_task_complete", "v:pp", false),
    ("slang_task_is_done", "b:p", false),
    ("slang_task_add_continuation", "b:ppp", false),
    ("slang_task_result", "p:p", false),
    ("slang_task_await", "p:p", false),
    ("slang_task_any", "p:pl", false),
    ("slang_runtime_init", "v:ip", false),
    ("slang_runtime_shutdown", "v:", false),
    ("slang_runtime_drain", "v:", false),
    // debug agent hooks (debug_agent.rs, ABI.md §22)
    ("slang_dbg_enter", "v:pppi", false),
    ("slang_dbg_stmt", "v:pi", false),
    ("slang_dbg_leave", "v:p", false),
    ("slang_dbg_suspend", "v:p", false),
    ("slang_dbg_resume", "v:p", false),
    ("slang_dbg_boot", "v:pp", false),
    ("slang_dbg_exit", "v:i", false),
];

impl<'a, 'ctx> CodeGen<'a, 'ctx> {
    fn sig_ty(&self, c: char) -> Option<BasicTypeEnum<'ctx>> {
        Some(match c {
            'p' => self.ptr_ty().into(),
            'l' => self.i64_ty().into(),
            'i' => self.ctx.i32_type().into(),
            'b' => self.ctx.bool_type().into(),
            'd' => self.ctx.f64_type().into(),
            'f' => self.ctx.f32_type().into(),
            'c' => self.ctx.i8_type().into(),
            _ => return None,
        })
    }

    /// The declaration of runtime function `name` (declared on first use).
    pub(crate) fn rt(&mut self, name: &str) -> FunctionValue<'ctx> {
        if let Some(f) = self.module.get_function(name) {
            return f;
        }
        let (_, sig, noret) = RUNTIME.iter().find(|r| r.0 == name).unwrap_or_else(|| panic!("codegen: unknown runtime function {name}"));
        let (ret, params) = sig.split_once(':').unwrap();
        let ps: Vec<char> = params.chars().filter(|c| !c.is_whitespace()).collect();
        let pts: Vec<BasicMetadataTypeEnum> = ps.iter().map(|c| self.sig_ty(*c).unwrap().into()).collect();
        let rc = ret.chars().next().unwrap();
        let fty = match self.sig_ty(rc) {
            None => self.ctx.void_type().fn_type(&pts, false),
            Some(t) => t.fn_type(&pts, false),
        };
        let f = self.module.add_function(name, fty, Some(Linkage::External));
        let zext = self.attr("zeroext");
        for (i, c) in ps.iter().enumerate() {
            if *c == 'b' {
                f.add_attribute(AttributeLoc::Param(i as u32), zext);
            }
        }
        if rc == 'b' {
            f.add_attribute(AttributeLoc::Return, zext);
        }
        if *noret {
            f.add_attribute(AttributeLoc::Function, self.attr("noreturn"));
            f.add_attribute(AttributeLoc::Function, self.attr("cold"));
        }
        f
    }

    pub(crate) fn attr(&self, name: &str) -> Attribute {
        self.ctx.create_enum_attribute(Attribute::get_named_enum_kind_id(name), 0)
    }

    /// Calls `f`, adding `zeroext` to every `i1` argument and result (C ABI).
    pub(crate) fn call_c(&mut self, f: FunctionValue<'ctx>, args: &[BasicMetadataValueEnum<'ctx>]) -> Option<BasicValueEnum<'ctx>> {
        let cs = self.b.build_call(f, args, "").unwrap();
        let zext = self.attr("zeroext");
        for (i, a) in args.iter().enumerate() {
            if let BasicMetadataValueEnum::IntValue(iv) = a {
                if iv.get_type().get_bit_width() == 1 {
                    cs.add_attribute(AttributeLoc::Param(i as u32), zext);
                }
            }
        }
        let r = cs.try_as_basic_value().left();
        if let Some(BasicValueEnum::IntValue(iv)) = r {
            if iv.get_type().get_bit_width() == 1 {
                cs.add_attribute(AttributeLoc::Return, zext);
            }
        }
        r
    }

    /// Calls runtime function `name`.
    pub(crate) fn call_rt(&mut self, name: &str, args: &[BasicMetadataValueEnum<'ctx>]) -> Option<BasicValueEnum<'ctx>> {
        if let Some(f) = self.rc_inline_fn(name) {
            self.b.build_call(f, args, "").unwrap();
            return None;
        }
        let f = self.rt(name);
        self.call_c(f, args)
    }

    /// Calls a runtime function that returns a value.
    pub(crate) fn call_rt_v(&mut self, name: &str, args: &[BasicMetadataValueEnum<'ctx>]) -> BasicValueEnum<'ctx> {
        self.call_rt(name, args).unwrap_or_else(|| panic!("codegen: {name} returns void"))
    }

    /// Calls a `noreturn` runtime function and continues in a fresh
    /// (unreachable) block.
    pub(crate) fn call_noreturn(&mut self, name: &str, args: &[BasicMetadataValueEnum<'ctx>]) {
        self.call_rt(name, args);
        self.b.build_unreachable().unwrap();
        self.dead_block();
    }

    /// Address of an external runtime constant (`slang_type_string`,
    /// `slang_ops_ref`, ...).
    pub(crate) fn rt_global(&mut self, name: &str) -> inkwell::values::PointerValue<'ctx> {
        if let Some(g) = self.module.get_global(name) {
            return g.as_pointer_value();
        }
        let ty = self.ctx.struct_type(&[self.ptr_ty().into(), self.i64_ty().into(), self.ptr_ty().into()], false);
        let g = self.module.add_global(ty, None, name);
        g.set_linkage(Linkage::External);
        g.set_constant(true);
        g.as_pointer_value()
    }
}
