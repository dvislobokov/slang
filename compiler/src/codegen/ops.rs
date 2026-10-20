//! Operators, conversions and built-in operations.

use super::{unsupported, CodeGen, Val, R};
use crate::hir::{BinOp, Builtin, ConvKind, Expr, UnOp};
use crate::types::{Type, TypeId};
use inkwell::values::{BasicValueEnum, IntValue};
use inkwell::{FloatPredicate, IntPredicate};

impl<'a, 'ctx> CodeGen<'a, 'ctx> {
    pub(crate) fn unary(&mut self, op: UnOp, x: &Expr) -> R<Val<'ctx>> {
        let v = self.expr(x)?.get();
        Ok(Val::borrowed(match op {
            UnOp::Not => self.b.build_not(v.into_int_value(), "").unwrap().into(),
            UnOp::Neg => {
                if v.is_float_value() {
                    self.b.build_float_neg(v.into_float_value(), "").unwrap().into()
                } else {
                    self.b.build_int_neg(v.into_int_value(), "").unwrap().into()
                }
            }
        }))
    }

    pub(crate) fn binary(&mut self, op: BinOp, lhs: &Expr, rhs: &Expr) -> R<Val<'ctx>> {
        match op {
            BinOp::And | BinOp::Or => return self.short_circuit(op, lhs, rhs),
            _ => {}
        }
        let l = self.expr(lhs)?;
        let r = self.expr(rhs)?;
        let v = self.binop_vals(op, lhs.ty, l.get(), r.get(), rhs.ty)?;
        self.drop_val(lhs.ty, l);
        self.drop_val(rhs.ty, r);
        Ok(v)
    }

    /// Binary operation on evaluated operands (also used by `op=`).
    pub(crate) fn binop_vals(&mut self, op: BinOp, ty: TypeId, l: BasicValueEnum<'ctx>, r: BasicValueEnum<'ctx>, rty: TypeId) -> R<Val<'ctx>> {
        let b = &self.b;
        let bool_ = |v: IntValue<'ctx>| Val::borrowed(v.into());
        match op {
            BinOp::StrConcat => return Ok(Val::owned(self.call_rt_v("slang_string_concat", &[l.into(), r.into()]))),
            BinOp::StrEq | BinOp::StrNe => {
                let eq = self.call_rt_v("slang_string_eq", &[l.into(), r.into()]).into_int_value();
                return Ok(bool_(if op == BinOp::StrEq { eq } else { self.b.build_not(eq, "").unwrap() }));
            }
            BinOp::StrLt | BinOp::StrLe | BinOp::StrGt | BinOp::StrGe => {
                let c = self.call_rt_v("slang_string_compare", &[l.into(), r.into()]).into_int_value();
                let pred = match op {
                    BinOp::StrLt => IntPredicate::SLT,
                    BinOp::StrLe => IntPredicate::SLE,
                    BinOp::StrGt => IntPredicate::SGT,
                    _ => IntPredicate::SGE,
                };
                let z = self.ctx.i32_type().const_zero();
                return Ok(bool_(self.b.build_int_compare(pred, c, z, "").unwrap()));
            }
            _ => {}
        }
        if l.is_float_value() {
            let (x, y) = (l.into_float_value(), r.into_float_value());
            let v: BasicValueEnum = match op {
                BinOp::Add => b.build_float_add(x, y, "").unwrap().into(),
                BinOp::Sub => b.build_float_sub(x, y, "").unwrap().into(),
                BinOp::Mul => b.build_float_mul(x, y, "").unwrap().into(),
                BinOp::Div => b.build_float_div(x, y, "").unwrap().into(),
                BinOp::Rem => b.build_float_rem(x, y, "").unwrap().into(),
                BinOp::Eq => b.build_float_compare(FloatPredicate::OEQ, x, y, "").unwrap().into(),
                BinOp::Ne => b.build_float_compare(FloatPredicate::UNE, x, y, "").unwrap().into(),
                BinOp::Lt => b.build_float_compare(FloatPredicate::OLT, x, y, "").unwrap().into(),
                BinOp::Le => b.build_float_compare(FloatPredicate::OLE, x, y, "").unwrap().into(),
                BinOp::Gt => b.build_float_compare(FloatPredicate::OGT, x, y, "").unwrap().into(),
                BinOp::Ge => b.build_float_compare(FloatPredicate::OGE, x, y, "").unwrap().into(),
                _ => return unsupported(format!("operator {op:?} on floats")),
            };
            return Ok(Val::borrowed(v));
        }
        // Pointers / interfaces / closures compared by address.
        let (x, y) = match (l, r) {
            (BasicValueEnum::IntValue(x), BasicValueEnum::IntValue(y)) => (x, y),
            (BasicValueEnum::PointerValue(x), BasicValueEnum::PointerValue(y)) => {
                (self.b.build_ptr_to_int(x, self.i64_ty(), "").unwrap(), self.b.build_ptr_to_int(y, self.i64_ty(), "").unwrap())
            }
            (BasicValueEnum::StructValue(x), BasicValueEnum::StructValue(y)) if matches!(op, BinOp::Eq | BinOp::Ne) && x.get_type().count_fields() == 2 => {
                let mut acc = self.ctx.bool_type().const_int(1, false);
                for i in 0..2 {
                    let a = self.b.build_extract_value(x, i, "").unwrap();
                    let c = self.b.build_extract_value(y, i, "").unwrap();
                    if !a.is_pointer_value() {
                        return unsupported("comparison of these values");
                    }
                    let ai = self.b.build_ptr_to_int(a.into_pointer_value(), self.i64_ty(), "").unwrap();
                    let ci = self.b.build_ptr_to_int(c.into_pointer_value(), self.i64_ty(), "").unwrap();
                    let e = self.b.build_int_compare(IntPredicate::EQ, ai, ci, "").unwrap();
                    acc = self.b.build_and(acc, e, "").unwrap();
                }
                if op == BinOp::Ne {
                    acc = self.b.build_not(acc, "").unwrap();
                }
                return Ok(bool_(acc));
            }
            _ => return unsupported(format!("operator {op:?} on {}", self.type_name(ty))),
        };
        let signed = self.is_signed(ty);
        let b = &self.b;
        let v: IntValue = match op {
            BinOp::Add => b.build_int_add(x, y, "").unwrap(),
            BinOp::Sub => b.build_int_sub(x, y, "").unwrap(),
            BinOp::Mul => b.build_int_mul(x, y, "").unwrap(),
            BinOp::Div | BinOp::Rem => return Ok(Val::borrowed(self.int_divrem(op, x, y, signed).into())),
            BinOp::BitAnd => b.build_and(x, y, "").unwrap(),
            BinOp::BitOr => b.build_or(x, y, "").unwrap(),
            BinOp::BitXor => b.build_xor(x, y, "").unwrap(),
            BinOp::Shl | BinOp::Shr => return Ok(Val::borrowed(self.shift(op, x, y, signed, self.is_signed(rty)).into())),
            BinOp::Eq => b.build_int_compare(IntPredicate::EQ, x, y, "").unwrap(),
            BinOp::Ne => b.build_int_compare(IntPredicate::NE, x, y, "").unwrap(),
            BinOp::Lt => b.build_int_compare(if signed { IntPredicate::SLT } else { IntPredicate::ULT }, x, y, "").unwrap(),
            BinOp::Le => b.build_int_compare(if signed { IntPredicate::SLE } else { IntPredicate::ULE }, x, y, "").unwrap(),
            BinOp::Gt => b.build_int_compare(if signed { IntPredicate::SGT } else { IntPredicate::UGT }, x, y, "").unwrap(),
            BinOp::Ge => b.build_int_compare(if signed { IntPredicate::SGE } else { IntPredicate::UGE }, x, y, "").unwrap(),
            _ => return unsupported(format!("operator {op:?}")),
        };
        Ok(Val::borrowed(v.into()))
    }

    /// Go integer division: panics on zero; `MIN / -1` wraps.
    fn int_divrem(&mut self, op: BinOp, x: IntValue<'ctx>, y: IntValue<'ctx>, signed: bool) -> IntValue<'ctx> {
        let t = x.get_type();
        let zero = self.b.build_int_compare(IntPredicate::EQ, y, t.const_zero(), "").unwrap();
        let bad = self.block("div0");
        let ok = self.block("divok");
        self.b.build_conditional_branch(zero, bad, ok).unwrap();
        self.b.position_at_end(bad);
        let msg = self.cstr("runtime error: integer divide by zero");
        self.call_rt("slang_panic", &[msg.into()]);
        self.b.build_unreachable().unwrap();
        self.b.position_at_end(ok);
        if !signed {
            return if op == BinOp::Div { self.b.build_int_unsigned_div(x, y, "").unwrap() } else { self.b.build_int_unsigned_rem(x, y, "").unwrap() };
        }
        // Avoid the UB of MIN / -1: divide by 1 instead (MIN / -1 == -MIN == MIN, MIN % -1 == 0).
        let m1 = self.b.build_int_compare(IntPredicate::EQ, y, t.const_all_ones(), "").unwrap();
        let ysafe = self.b.build_select(m1, t.const_int(1, false), y, "").unwrap().into_int_value();
        if op == BinOp::Div {
            let q = self.b.build_int_signed_div(x, ysafe, "").unwrap();
            let neg = self.b.build_int_neg(x, "").unwrap();
            self.b.build_select(m1, neg, q, "").unwrap().into_int_value()
        } else {
            let r = self.b.build_int_signed_rem(x, ysafe, "").unwrap();
            self.b.build_select(m1, t.const_zero(), r, "").unwrap().into_int_value()
        }
    }

    /// Go shifts: counts >= width give 0 (or the sign for `>>` on signed).
    fn shift(&mut self, op: BinOp, x: IntValue<'ctx>, y: IntValue<'ctx>, signed: bool, rsigned: bool) -> IntValue<'ctx> {
        let t = x.get_type();
        let bits = t.get_bit_width();
        let yb = y.get_type().get_bit_width();
        let y64 = if yb < 64 {
            if rsigned {
                self.b.build_int_s_extend(y, self.i64_ty(), "").unwrap()
            } else {
                self.b.build_int_z_extend(y, self.i64_ty(), "").unwrap()
            }
        } else {
            y
        };
        let big = self.b.build_int_compare(IntPredicate::UGE, y64, self.i64_ty().const_int(bits as u64, false), "").unwrap();
        let cnt = if bits < 64 { self.b.build_int_truncate(y64, t, "").unwrap() } else { y64 };
        let safe = self.b.build_select(big, t.const_zero(), cnt, "").unwrap().into_int_value();
        if op == BinOp::Shl {
            let v = self.b.build_left_shift(x, safe, "").unwrap();
            self.b.build_select(big, t.const_zero(), v, "").unwrap().into_int_value()
        } else if signed {
            let maxs = self.b.build_select(big, t.const_int(bits as u64 - 1, false), safe, "").unwrap().into_int_value();
            self.b.build_right_shift(x, maxs, true, "").unwrap()
        } else {
            let v = self.b.build_right_shift(x, safe, false, "").unwrap();
            self.b.build_select(big, t.const_zero(), v, "").unwrap().into_int_value()
        }
    }

    fn short_circuit(&mut self, op: BinOp, lhs: &Expr, rhs: &Expr) -> R<Val<'ctx>> {
        let res = self.entry_alloca(self.ctx.bool_type().into(), "sc", false);
        let l = self.expr(lhs)?.get().into_int_value();
        self.b.build_store(res, l).unwrap();
        let rhs_bb = self.block("sc.rhs");
        let done = self.block("sc.done");
        if op == BinOp::And {
            self.b.build_conditional_branch(l, rhs_bb, done).unwrap();
        } else {
            self.b.build_conditional_branch(l, done, rhs_bb).unwrap();
        }
        self.b.position_at_end(rhs_bb);
        let r = self.expr(rhs)?.get().into_int_value();
        self.b.build_store(res, r).unwrap();
        self.br(done);
        self.b.position_at_end(done);
        Ok(Val::borrowed(self.b.build_load(self.ctx.bool_type(), res, "").unwrap()))
    }

    /// True if `v` (of type `t`) is nil.
    pub(crate) fn nil_test(&mut self, t: TypeId, v: BasicValueEnum<'ctx>) -> IntValue<'ctx> {
        let p = match self.ty(t) {
            Type::Slice(_) | Type::Func(_) => self.b.build_extract_value(v.into_struct_value(), 0, "").unwrap().into_pointer_value(),
            Type::Interface(_) | Type::Any => self.b.build_extract_value(v.into_struct_value(), 1, "").unwrap().into_pointer_value(),
            _ => v.into_pointer_value(),
        };
        self.b.build_is_null(p, "").unwrap()
    }

    pub(crate) fn is_nil(&mut self, value: &Expr, negated: bool) -> R<Val<'ctx>> {
        let v = self.expr(value)?;
        let mut r = self.nil_test(value.ty, v.get());
        if negated {
            r = self.b.build_not(r, "").unwrap();
        }
        self.drop_val(value.ty, v);
        Ok(Val::borrowed(r.into()))
    }

    pub(crate) fn convert(&mut self, kind: ConvKind, value: &Expr, to: TypeId) -> R<Val<'ctx>> {
        let v = self.expr(value)?;
        let x = v.get();
        let from = value.ty;
        let tt = self.llty(to);
        let r: BasicValueEnum = match kind {
            ConvKind::IntToInt | ConvKind::IntToChar | ConvKind::CharToInt | ConvKind::EnumToInt => {
                let xi = x.into_int_value();
                let src_signed = match kind {
                    ConvKind::EnumToInt => false,
                    ConvKind::CharToInt => true,
                    _ => self.is_signed(from),
                };
                self.b.build_int_cast_sign_flag(xi, tt.into_int_type(), src_signed, "").unwrap().into()
            }
            ConvKind::IntToFloat => {
                let xi = x.into_int_value();
                if self.is_signed(from) {
                    self.b.build_signed_int_to_float(xi, tt.into_float_type(), "").unwrap().into()
                } else {
                    self.b.build_unsigned_int_to_float(xi, tt.into_float_type(), "").unwrap().into()
                }
            }
            ConvKind::FloatToInt => {
                let xf = x.into_float_value();
                if self.is_signed(to) {
                    self.b.build_float_to_signed_int(xf, tt.into_int_type(), "").unwrap().into()
                } else {
                    self.b.build_float_to_unsigned_int(xf, tt.into_int_type(), "").unwrap().into()
                }
            }
            ConvKind::FloatToFloat => self.b.build_float_cast(x.into_float_value(), tt.into_float_type(), "").unwrap().into(),
            ConvKind::CharToString => return Ok(Val::owned(self.call_rt_v("slang_string_from_rune", &[x.into()]))),
        };
        Ok(Val::borrowed(r))
    }

    pub(crate) fn builtin(&mut self, ty: TypeId, op: Builtin, args: &[Expr]) -> R<Val<'ctx>> {
        match op {
            Builtin::LenString => {
                let s = self.expr(&args[0])?;
                let r = self.call_rt_v("slang_string_len", &[s.get().into()]);
                self.drop_val(args[0].ty, s);
                Ok(Val::borrowed(r))
            }
            Builtin::LenSlice => {
                let s = self.expr(&args[0])?;
                let r = self.b.build_extract_value(s.get().into_struct_value(), 1, "len").unwrap();
                self.drop_val(args[0].ty, s);
                Ok(Val::borrowed(r))
            }
            Builtin::LenArray => {
                let n = match self.ty(args[0].ty) {
                    Type::Array(_, n) => *n,
                    _ => 0,
                };
                if !self.is_place(&args[0]) {
                    self.expr_discard(&args[0])?;
                }
                Ok(Val::borrowed(self.i64_ty().const_int(n, false).into()))
            }
            Builtin::LenMap => {
                let m = self.expr(&args[0])?;
                let r = self.call_rt_v("slang_map_len", &[m.get().into()]);
                self.drop_val(args[0].ty, m);
                Ok(Val::borrowed(r))
            }
            Builtin::Append => {
                let et = self.elem_of(ty).unwrap();
                let s = self.expr_owned(&args[0])?;
                let sp = self.spill(s);
                let x = self.expr(&args[1])?;
                let xp = self.spill(x.get());
                let size = self.i64_ty().const_int(self.size_of(et), false);
                let ops = self.value_ops(et);
                self.call_rt("slang_slice_append", &[sp.into(), size.into(), ops.into(), xp.into()]);
                self.drop_val(et, x);
                Ok(Val::owned(self.b.build_load(self.slice_ty(), sp, "").unwrap()))
            }
            Builtin::Delete => {
                let (kt, _) = self.map_kv(args[0].ty);
                let m = self.expr(&args[0])?;
                let k = self.expr(&args[1])?;
                match self.key_kind(kt) {
                    super::expr::KeyKind::Str => self.call_rt("slang_map_delete_str", &[m.get().into(), k.get().into()]),
                    super::expr::KeyKind::Int => self.call_rt("slang_map_delete_int", &[m.get().into(), k.get().into()]),
                    super::expr::KeyKind::Generic => {
                        let kp = self.spill(k.get());
                        self.call_rt("slang_map_delete", &[m.get().into(), kp.into()])
                    }
                };
                self.drop_val(args[1].ty, k);
                self.drop_val(args[0].ty, m);
                Ok(Val::void())
            }
            Builtin::Panic => {
                let v = self.expr(&args[0])?;
                let f = if *self.ty(args[0].ty) == Type::String { "slang_panic_str" } else { "slang_panic_error" };
                self.call_noreturn(f, &[v.get().into()]);
                Ok(Val::void())
            }
            Builtin::ErrorNew => {
                let m = self.expr(&args[0])?;
                let r = self.call_rt_v("slang_error_new", &[m.get().into()]);
                self.drop_val(args[0].ty, m);
                Ok(Val::owned(r))
            }
            Builtin::ErrorsAs => self.errors_as(&args[0], &args[1]),
        }
    }
}
