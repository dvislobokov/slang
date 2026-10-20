//! Expression checking and lowering (everything except calls, which live
//! in `call.rs`, and `try`, which lives in `stmt.rs`).

use super::coerce::Ctx;
use super::resolve::PathRes;
use super::*;
use crate::hir::{BinOp, ExprKind, UnOp};

impl<'a> Sema<'a> {
    /// Checks an expression; the result may be fallible (`T!`) or `void`.
    /// `expected` guides untyped constants, `nil` and empty literals.
    pub(crate) fn expr(&mut self, e: &ast::Expr, expected: Option<TypeId>) -> hir::Expr {
        use ast::ExprKind as K;
        if let Some(f) = self.fold(e) {
            return self.const_expr(f, expected, e.span);
        }
        let span = e.span;
        match &e.kind {
            K::Int(_) | K::Float(_) | K::Str(_) | K::Char(_) | K::Bool(_) => unreachable!("literals always fold"),
            K::Nil => mk(ExprKind::Nil, self.tt.nil, span),
            K::This => match self.lookup_local("this", true) {
                Some(Ok(v)) => self.var_expr(v, span),
                _ => {
                    self.err("this used outside of a method", span);
                    self.invalid(span)
                }
            },
            K::Ident(_) | K::Field { .. } => match Self::flatten_path(e) {
                Some(segs) => self.path_expr(&segs, span, expected),
                None => {
                    let K::Field { base, name } = &e.kind else { unreachable!() };
                    let b = self.value(base, None);
                    self.field_access(b, name)
                }
            },
            K::Call { callee, args } => self.call(callee, args, span, expected),
            K::Index { base, index } => self.index(base, index, span),
            K::Unary { op, expr } => self.unary(*op, expr, span, expected),
            K::Binary { op, left, right } => self.binary(*op, left, right, span, expected),
            K::Propagate { expr, context } => self.propagate(expr, context.as_deref(), span),
            K::Try { .. } => self.try_expr(e, true),
            K::Await(inner) => self.await_expr(inner, span),
            K::StructLit { path, generics, fields } => self.struct_lit(path, generics, fields, span),
            K::TypeArgs { .. } => {
                self.err("generics are not supported yet", span);
                self.invalid(span)
            }
            K::New(inner) => self.new_expr(inner, span),
            K::SliceLit(elems) => self.slice_lit(elems, span, expected),
            K::MapLit { key_ty, val_ty, entries } => {
                let k = self.resolve_type(key_ty);
                let v = self.resolve_type(val_ty);
                if !self.is_map_key(k) {
                    self.err(format!("invalid map key type {}", self.tn(k)), key_ty.span);
                }
                let ty = self.tt.map(k, v);
                let entries = entries.iter().map(|(ke, ve)| (self.expr_to(ke, k, Ctx::Elem), self.expr_to(ve, v, Ctx::Elem))).collect();
                mk(ExprKind::MapLit(entries), ty, span)
            }
            K::Tuple(es) => {
                let hint: Vec<Option<TypeId>> = match expected.map(|t| self.tt.get(t).clone()) {
                    Some(Type::Tuple(ts)) if ts.len() == es.len() => ts.into_iter().map(Some).collect(),
                    _ => vec![None; es.len()],
                };
                let vs: Vec<hir::Expr> = es.iter().zip(hint).map(|(x, h)| {
                    let v = self.value(x, h);
                    self.default_type(v)
                }).collect();
                let ty = self.tt.tuple(vs.iter().map(|v| v.ty).collect());
                mk(ExprKind::Tuple(vs), ty, span)
            }
            K::Range(..) => {
                self.err("a range a..b is only allowed in for, as an index, or as a case pattern", span);
                self.invalid(span)
            }
            K::Lambda { params, ret, body } => self.lambda(params, ret.as_deref(), body, span),
            K::Paren(x) => self.expr(x, expected),
        }
    }

    /// Checks an expression used as a value: rejects unhandled `T!` and
    /// `void`.
    pub(crate) fn value(&mut self, e: &ast::Expr, expected: Option<TypeId>) -> hir::Expr {
        let h = self.expr(e, expected);
        self.require_value(h)
    }

    /// Rejects unhandled fallible results and void values.
    pub(crate) fn require_value(&mut self, h: hir::Expr) -> hir::Expr {
        if self.tt.is_fallible(h.ty) {
            self.err(format!("result of type {} must be handled (use ?, try ... catch, try ... else, or `_ =`)", self.tn(h.ty)), h.span);
            return mk(h.kind, self.tt.invalid, h.span);
        }
        if matches!(self.tt.get(h.ty), Type::Void) {
            self.err("expression of type void used as a value", h.span);
            return mk(h.kind, self.tt.invalid, h.span);
        }
        h
    }

    /// Checks `e` as a value of type `target` (with implicit conversions).
    pub(crate) fn expr_to(&mut self, e: &ast::Expr, target: TypeId, ctx: Ctx) -> hir::Expr {
        let h = self.value(e, Some(target));
        self.coerce(h, target, ctx)
    }

    pub(crate) fn invalid(&self, span: Span) -> hir::Expr {
        mk(ExprKind::Zero, self.tt.invalid, span)
    }

    pub(crate) fn var_expr(&self, v: VarRef, span: Span) -> hir::Expr {
        let ty = self.var_ty(v);
        match v {
            VarRef::Local(l) => mk(ExprKind::Local(l), ty, span),
            VarRef::Upvalue(i) => mk(ExprKind::Upvalue(i), ty, span),
        }
    }

    /// Function value type of a user function.
    pub(crate) fn func_value_ty(&mut self, f: FuncId) -> TypeId {
        let sig = self.sigs[f.0 as usize].clone();
        let ret = if sig.is_async { self.tt.task(sig.ret) } else { sig.ret };
        self.tt.func(sig.params, ret)
    }

    /// `a`, `a.b.c`: locals, globals, namespaces, enum variants, fields.
    pub(crate) fn path_expr(&mut self, segs: &[&ast::Ident], span: Span, expected: Option<TypeId>) -> hir::Expr {
        let (mut base, consumed) = match self.lookup_local(&segs[0].name, true) {
            Some(Ok(v)) => (self.var_expr(v, segs[0].span), 1),
            Some(Err(c)) => (self.const_expr(c, expected, segs[0].span), 1),
            None => match self.resolve_segments(segs, true) {
                Some((res, n)) => {
                    let sp = segs[0].span.to(segs[n - 1].span);
                    (self.path_value(res, &segs[n - 1].name, sp, expected), n)
                }
                None => {
                    if self.lookup_global(segs[0], false).is_none() && !self.diags.has_errors_at(segs[0].span, self.cur_file) {
                        self.err(format!("unknown identifier {}", segs[0].name), segs[0].span);
                    }
                    return self.invalid(span);
                }
            },
        };
        for seg in &segs[consumed..] {
            base = self.field_access(base, seg);
        }
        base
    }

    /// The value named by a symbolic path resolution.
    pub(crate) fn path_value(&mut self, res: PathRes, name: &str, span: Span, expected: Option<TypeId>) -> hir::Expr {
        match res {
            PathRes::Member(Member::Global(g)) => {
                self.ensure_global(g);
                if let Some(f) = self.global_folded.get(&g.0).cloned() {
                    return self.const_expr(f, expected, span);
                }
                let ty = self.globals[g.0 as usize].ty;
                mk(ExprKind::Global(g), ty, span)
            }
            PathRes::Member(Member::Func(f)) | PathRes::StaticMethod(f) => {
                let ty = self.func_value_ty(f);
                mk(ExprKind::FuncRef(f), ty, span)
            }
            PathRes::Member(Member::StdConst(i)) => {
                let f = Folded { value: hir::ConstValue::Float(stdlib::CONSTS[i].2), ty: None };
                self.const_expr(f, expected, span)
            }
            PathRes::Variant(e, v) => {
                let ty = self.enum_ty(e);
                let var = &self.enums[e as usize].variants[v as usize];
                if !var.fields.is_empty() {
                    let msg = format!("variant {name} has fields; construct it with {}.{name}(...)", self.enums[e as usize].name);
                    self.err(msg, span);
                }
                mk(ExprKind::EnumLit { enum_id: e, variant: v, args: Vec::new() }, ty, span)
            }
            PathRes::Member(Member::StdFunc(_) | Member::Special(_)) | PathRes::Builtin(_) => {
                self.err(format!("builtin or library function {name} must be called (it cannot be used as a value)"), span);
                self.invalid(span)
            }
            PathRes::Member(Member::Ns(_)) => {
                self.err(format!("{name} is a namespace, not a value"), span);
                self.invalid(span)
            }
            PathRes::Member(Member::Struct(_) | Member::Interface(_) | Member::Enum(_)) | PathRes::BuiltinType(_) => {
                self.err(format!("{name} is a type, not a value"), span);
                self.invalid(span)
            }
        }
    }

    /// `base.name` on a value: struct fields (with auto-deref and embedded
    /// promotion).
    pub(crate) fn field_access(&mut self, base: hir::Expr, name: &ast::Ident) -> hir::Expr {
        if self.tt.is_invalid(base.ty) {
            return self.invalid(name.span);
        }
        let span = base.span.to(name.span);
        let mut b = self.auto_deref(base);
        let Type::Struct(d) = *self.tt.get(b.ty) else {
            let t = self.tn(b.ty);
            self.err(format!("type {t} has no field {} (field access needs a struct or pointer to struct)", name.name), name.span);
            return self.invalid(span);
        };
        match self.find_field(d.0, &name.name) {
            Some(path) => {
                for idx in path {
                    let b2 = self.auto_deref(b);
                    let Type::Struct(sd) = *self.tt.get(b2.ty) else { unreachable!() };
                    let field = &self.structs[sd.0 as usize].fields[idx as usize];
                    let fty = field.ty;
                    let fname = field.name.clone();
                    let owner = self.structs[sd.0 as usize].namespace;
                    self.check_member_visibility(owner, &fname, name.span, true);
                    b = mk(ExprKind::Field { base: Box::new(b2), index: idx }, fty, span);
                }
                b
            }
            None => {
                let sname = self.structs[d.0 as usize].name.clone();
                if self.find_method(d.0, &name.name).is_some() {
                    self.err(format!("method {sname}.{} must be called (method values are not supported)", name.name), name.span);
                } else {
                    self.err(format!("struct {sname} has no field or method {}", name.name), name.span);
                }
                self.invalid(span)
            }
        }
    }

    /// Inserts `Deref` for a pointer to a struct.
    pub(crate) fn auto_deref(&mut self, e: hir::Expr) -> hir::Expr {
        if let Type::Pointer(inner) = *self.tt.get(e.ty) {
            if matches!(self.tt.get(inner), Type::Struct(_)) {
                let span = e.span;
                return mk(ExprKind::Deref(Box::new(e)), inner, span);
            }
        }
        e
    }

    /// True for expressions that denote storage. `MapGet` counts only when
    /// `allow_map` (assignment targets).
    pub(crate) fn is_place(&self, e: &hir::Expr, allow_map: bool) -> bool {
        match &e.kind {
            ExprKind::Local(_) | ExprKind::Upvalue(_) | ExprKind::Deref(_) => true,
            ExprKind::Global(g) => !self.globals[g.0 as usize].is_const,
            ExprKind::Field { base, .. } => self.is_place(base, false),
            ExprKind::Index { base, .. } => matches!(self.tt.get(base.ty), Type::Slice(_)) || self.is_place(base, false),
            ExprKind::MapGet { .. } => allow_map,
            _ => false,
        }
    }

    /// `&e` for a method receiver or `&x`.
    pub(crate) fn addr_of(&mut self, e: hir::Expr) -> hir::Expr {
        self.mark_address_taken(&e);
        let ty = self.tt.pointer(e.ty);
        let span = e.span;
        mk(ExprKind::AddrOf(Box::new(e)), ty, span)
    }

    fn index(&mut self, base: &ast::Expr, index: &ast::Expr, span: Span) -> hir::Expr {
        let b = self.value(base, None);
        if self.tt.is_invalid(b.ty) {
            return self.invalid(span);
        }
        if let ast::ExprKind::Range(lo, hi) = &index.kind {
            let ty = match *self.tt.get(b.ty) {
                Type::Slice(_) | Type::String => b.ty,
                Type::Array(t, _) => {
                    if !self.is_place(&b, false) {
                        self.err("cannot slice an array that is not stored in a variable", b.span);
                    }
                    self.mark_address_taken(&b);
                    self.tt.slice(t)
                }
                _ => {
                    self.err(format!("cannot slice value of type {}", self.tn(b.ty)), b.span);
                    return self.invalid(span);
                }
            };
            let lo = lo.as_ref().map(|x| Box::new(self.int_index(x)));
            let hi = hi.as_ref().map(|x| Box::new(self.int_index(x)));
            return mk(ExprKind::SubSlice { base: Box::new(b), lo, hi }, ty, span);
        }
        match self.tt.get(b.ty).clone() {
            Type::Slice(t) | Type::Array(t, _) => {
                let i = self.int_index(index);
                mk(ExprKind::Index { base: Box::new(b), index: Box::new(i) }, t, span)
            }
            Type::Map(k, v) => {
                let key = self.expr_to(index, k, Ctx::Other);
                mk(ExprKind::MapGet { map: Box::new(b), key: Box::new(key) }, v, span)
            }
            Type::String => {
                self.err("string indexing is not supported (iterate with `for ch in s`)", span);
                self.invalid(span)
            }
            _ => {
                self.err(format!("cannot index value of type {}", self.tn(b.ty)), b.span);
                self.invalid(span)
            }
        }
    }

    fn int_index(&mut self, e: &ast::Expr) -> hir::Expr {
        let i = self.value(e, Some(self.tt.int));
        if !self.tt.is_integer(i.ty) && !self.tt.is_invalid(i.ty) {
            self.err(format!("index must be an integer, found {}", self.tn(i.ty)), i.span);
        }
        i
    }

    fn unary(&mut self, op: ast::UnaryOp, x: &ast::Expr, span: Span, expected: Option<TypeId>) -> hir::Expr {
        match op {
            ast::UnaryOp::Neg => {
                let v = self.value(x, expected);
                if !self.tt.is_numeric(v.ty) {
                    if !self.tt.is_invalid(v.ty) {
                        self.err(format!("operator - is not defined on {}", self.tn(v.ty)), span);
                    }
                    return self.invalid(span);
                }
                let ty = v.ty;
                mk(ExprKind::Unary { op: UnOp::Neg, operand: Box::new(v) }, ty, span)
            }
            ast::UnaryOp::Not => {
                let v = self.value(x, Some(self.tt.bool_));
                if v.ty != self.tt.bool_ && !self.tt.is_invalid(v.ty) {
                    self.err(format!("operator ! is not defined on {}", self.tn(v.ty)), span);
                }
                mk(ExprKind::Unary { op: UnOp::Not, operand: Box::new(v) }, self.tt.bool_, span)
            }
            ast::UnaryOp::AddrOf => {
                let inner = strip_parens(x);
                let v = self.value(x, None);
                if self.tt.is_invalid(v.ty) {
                    return self.invalid(span);
                }
                let is_lit = matches!(inner.kind, ast::ExprKind::StructLit { .. });
                if !is_lit && !self.is_place(&v, false) {
                    self.err("cannot take the address of this expression", span);
                }
                let mut r = self.addr_of(v);
                r.span = span;
                r
            }
            ast::UnaryOp::Deref => {
                let v = self.value(x, None);
                match *self.tt.get(v.ty) {
                    Type::Pointer(t) => mk(ExprKind::Deref(Box::new(v)), t, span),
                    Type::Invalid => self.invalid(span),
                    _ => {
                        self.err(format!("cannot dereference value of type {} (not a pointer)", self.tn(v.ty)), span);
                        self.invalid(span)
                    }
                }
            }
        }
    }

    /// True if `e` folds to an untyped constant or is `nil`.
    fn is_untyped(&mut self, e: &ast::Expr) -> bool {
        matches!(strip_parens(e).kind, ast::ExprKind::Nil) || self.fold(e).is_some_and(|f| f.ty.is_none())
    }

    /// Checks two operands so that an untyped one takes the other's type.
    pub(crate) fn operands(&mut self, l: &ast::Expr, r: &ast::Expr, hint: Option<TypeId>) -> (hir::Expr, hir::Expr) {
        if self.is_untyped(l) && !self.is_untyped(r) {
            let rv = self.value(r, hint);
            let lv = self.value(l, Some(rv.ty));
            (lv, rv)
        } else {
            let lv = self.value(l, hint);
            let rv = self.value(r, Some(lv.ty));
            (lv, rv)
        }
    }

    fn binary(&mut self, op: ast::BinaryOp, l: &ast::Expr, r: &ast::Expr, span: Span, expected: Option<TypeId>) -> hir::Expr {
        use ast::BinaryOp as B;
        if matches!(op, B::And | B::Or) {
            let bt = self.tt.bool_;
            let lv = self.value(l, Some(bt));
            let rv = self.value(r, Some(bt));
            for v in [&lv, &rv] {
                if v.ty != bt && !self.tt.is_invalid(v.ty) {
                    self.err(format!("operator {} requires bool operands, found {}", if op == B::And { "&&" } else { "||" }, self.tn(v.ty)), v.span);
                }
            }
            let hop = if op == B::And { BinOp::And } else { BinOp::Or };
            return mk(ExprKind::Binary { op: hop, lhs: Box::new(lv), rhs: Box::new(rv) }, bt, span);
        }
        let hint = if matches!(op, B::Eq | B::Ne | B::Lt | B::Le | B::Gt | B::Ge) { None } else { expected.filter(|t| self.tt.is_numeric(*t)) };
        let (lv, rv) = self.operands(l, r, hint);
        self.make_binary(op, lv, rv, span)
    }

    /// Types a binary operation on checked operands.
    pub(crate) fn make_binary(&mut self, op: ast::BinaryOp, lv: hir::Expr, rv: hir::Expr, span: Span) -> hir::Expr {
        use ast::BinaryOp as B;
        if self.tt.is_invalid(lv.ty) || self.tt.is_invalid(rv.ty) {
            return self.invalid(span);
        }
        // nil comparisons
        let lnil = matches!(self.tt.get(lv.ty), Type::Nil);
        let rnil = matches!(self.tt.get(rv.ty), Type::Nil);
        if (lnil || rnil) && matches!(op, B::Eq | B::Ne) {
            let v = if lnil { rv } else { lv };
            if matches!(self.tt.get(v.ty), Type::Nil) || !self.tt.is_nilable(v.ty) {
                self.err(format!("cannot compare {} with nil", self.tn(v.ty)), span);
                return self.invalid(span);
            }
            return mk(ExprKind::IsNil { value: Box::new(v), negated: op == B::Ne }, self.tt.bool_, span);
        }
        if matches!(op, B::Div | B::Rem) && matches!(rv.kind, ExprKind::Const(hir::ConstValue::Int(0))) {
            self.err("division by zero", rv.span);
        }
        match self.binop_for(op, lv.ty, rv.ty, span) {
            Some((hop, ty)) => mk(ExprKind::Binary { op: hop, lhs: Box::new(lv), rhs: Box::new(rv) }, ty, span),
            None => self.invalid(span),
        }
    }

    /// Chooses the HIR operator and result type for `l op r`, reporting
    /// invalid combinations.
    pub(crate) fn binop_for(&mut self, op: ast::BinaryOp, l: TypeId, r: TypeId, span: Span) -> Option<(BinOp, TypeId)> {
        use ast::BinaryOp as B;
        let sym = op_symbol(op);
        if matches!(op, B::Shl | B::Shr) {
            if !self.tt.is_integer(l) || !self.tt.is_integer(r) {
                self.err(format!("operator {sym} requires integer operands, found {} and {}", self.tn(l), self.tn(r)), span);
                return None;
            }
            return Some((if op == B::Shl { BinOp::Shl } else { BinOp::Shr }, l));
        }
        if l != r {
            self.err(format!("mismatched types {} and {} for operator {sym} (no implicit conversion; use int(x) or float(x))", self.tn(l), self.tn(r)), span);
            return None;
        }
        let t = self.tt.get(l).clone();
        let numeric = self.tt.is_numeric(l);
        let integer = self.tt.is_integer(l);
        let is_str = matches!(t, Type::String);
        let bool_ = self.tt.bool_;
        let res = match op {
            B::Add if is_str => Some((BinOp::StrConcat, l)),
            B::Add | B::Sub | B::Mul | B::Div if numeric => Some((arith(op), l)),
            B::Rem | B::BitAnd | B::BitOr | B::BitXor if integer => Some((arith(op), l)),
            B::Eq | B::Ne => {
                let comparable = numeric
                    || matches!(t, Type::Bool | Type::Char | Type::Pointer(_))
                    || matches!(t, Type::Enum(d) if self.enums[d.0 as usize].is_plain);
                if is_str {
                    Some((if op == B::Eq { BinOp::StrEq } else { BinOp::StrNe }, bool_))
                } else if comparable {
                    Some((if op == B::Eq { BinOp::Eq } else { BinOp::Ne }, bool_))
                } else {
                    None
                }
            }
            B::Lt | B::Le | B::Gt | B::Ge => {
                if is_str {
                    Some((
                        match op {
                            B::Lt => BinOp::StrLt,
                            B::Le => BinOp::StrLe,
                            B::Gt => BinOp::StrGt,
                            _ => BinOp::StrGe,
                        },
                        bool_,
                    ))
                } else if numeric || matches!(t, Type::Char) {
                    Some((arith(op), bool_))
                } else {
                    None
                }
            }
            _ => None,
        };
        if res.is_none() {
            self.err(format!("operator {sym} is not defined on {}", self.tn(l)), span);
        }
        res
    }

    fn propagate(&mut self, x: &ast::Expr, context: Option<&ast::Expr>, span: Span) -> hir::Expr {
        let v = self.expr(x, None);
        let (ret, name, in_fn) = match self.fns.last() {
            Some(f) => (f.ret, f.name.clone(), f.func.is_some()),
            None => (self.tt.void, String::new(), false),
        };
        if !in_fn || !self.tt.is_fallible(ret) {
            self.err(format!("? outside of fallible function: {name} does not return a T! type (use try ... catch or try ... else instead)"), span);
        }
        let ctx = context.map(|c| Box::new(self.expr_to(c, self.tt.string, Ctx::Other)));
        let Some(inner) = self.tt.fallible_inner(v.ty) else {
            if !self.tt.is_invalid(v.ty) {
                self.err(format!("? requires a fallible (T!) operand, found {}", self.tn(v.ty)), v.span);
            }
            return self.invalid(span);
        };
        mk(ExprKind::Propagate { value: Box::new(v), context: ctx }, inner, span)
    }

    fn await_expr(&mut self, x: &ast::Expr, span: Span) -> hir::Expr {
        let (is_async, name) = self.fns.last().map(|f| (f.is_async, f.name.clone())).unwrap_or_default();
        if !is_async {
            self.err(format!("await outside async function: {name} is not declared async"), span);
        }
        let v = self.value(x, None);
        match *self.tt.get(v.ty) {
            Type::Task(t) => mk(ExprKind::Await(Box::new(v)), t, span),
            Type::Invalid => self.invalid(span),
            _ => {
                self.err(format!("await requires a Task (the result of an async function), found {}", self.tn(v.ty)), v.span);
                self.invalid(span)
            }
        }
    }

    /// Resolves the type named by a struct-literal path to a struct id.
    fn struct_lit_target(&mut self, path: &ast::Path) -> Option<u32> {
        let segs: Vec<&ast::Ident> = path.segments.iter().collect();
        if self.lookup_global(segs[0], false).is_none() {
            self.err(format!("unknown type {}", path.to_string()), path.span);
            return None;
        }
        let (res, n) = self.resolve_segments(&segs, true)?;
        match res {
            PathRes::Member(Member::Struct(s)) if n == segs.len() => Some(s),
            _ => {
                self.err(format!("{} is not a struct type", path.to_string()), path.span);
                None
            }
        }
    }

    fn struct_lit(&mut self, path: &ast::Path, generics: &[ast::TypeExpr], fields: &[ast::FieldInit], span: Span) -> hir::Expr {
        if !generics.is_empty() {
            self.err("generics are not supported yet", span);
        }
        let Some(sid) = self.struct_lit_target(path) else {
            for f in fields {
                self.value(&f.value, None);
            }
            return self.invalid(span);
        };
        let ty = self.struct_ty(sid);
        let owner = self.structs[sid as usize].namespace;
        let mut out: Vec<(u32, hir::Expr)> = Vec::new();
        for f in fields {
            let found = self.structs[sid as usize].fields.iter().position(|x| x.name == f.name.name);
            let Some(idx) = found else {
                let sname = self.structs[sid as usize].name.clone();
                self.err(format!("unknown field {} in struct literal of type {sname}", f.name.name), f.name.span);
                self.value(&f.value, None);
                continue;
            };
            if out.iter().any(|(i, _)| *i == idx as u32) {
                self.err(format!("duplicate field {} in struct literal", f.name.name), f.name.span);
            }
            self.check_member_visibility(owner, &f.name.name, f.name.span, true);
            let fty = self.structs[sid as usize].fields[idx].ty;
            let v = self.expr_to(&f.value, fty, Ctx::Field(f.name.name.clone()));
            out.push((idx as u32, v));
        }
        mk(ExprKind::StructLit { struct_id: sid, fields: out }, ty, span)
    }

    fn new_expr(&mut self, inner: &ast::Expr, span: Span) -> hir::Expr {
        if let ast::ExprKind::StructLit { .. } = inner.kind {
            let v = self.value(inner, None);
            let ty = self.tt.pointer(v.ty);
            return mk(ExprKind::New(Box::new(v)), ty, span);
        }
        if let Some(segs) = Self::flatten_path(inner) {
            if self.lookup_local(&segs[0].name, false).is_none() {
                if let Some((res, n)) = self.resolve_segments(&segs, false) {
                    if n == segs.len() {
                        if let Some(t) = self.path_res_type(res) {
                            let zero = mk(ExprKind::Zero, t, inner.span);
                            let ty = self.tt.pointer(t);
                            return mk(ExprKind::New(Box::new(zero)), ty, span);
                        }
                    }
                }
            }
        }
        self.err("new requires a struct literal or a type name", inner.span);
        self.invalid(span)
    }

    fn slice_lit(&mut self, elems: &[ast::Expr], span: Span, expected: Option<TypeId>) -> hir::Expr {
        if let Some(t) = expected {
            if let Type::Slice(el) = *self.tt.get(t) {
                let vs = elems.iter().map(|e| self.expr_to(e, el, Ctx::Elem)).collect();
                return mk(ExprKind::SliceLit(vs), t, span);
            }
        }
        let Some(first) = elems.first() else {
            self.err("cannot infer the type of an empty slice literal; declare the variable with a type (var s []int = [])", span);
            return self.invalid(span);
        };
        let f = self.value(first, None);
        let f = self.default_type(f);
        let el = f.ty;
        let mut vs = vec![f];
        for e in &elems[1..] {
            vs.push(self.expr_to(e, el, Ctx::Elem));
        }
        let ty = self.tt.slice(el);
        mk(ExprKind::SliceLit(vs), ty, span)
    }

    /// Lambda: lifted into a new function; returns the closure value.
    fn lambda(&mut self, params: &[ast::Param], ret: Option<&ast::TypeExpr>, body: &ast::LambdaBody, span: Span) -> hir::Expr {
        let Some(parent) = self.fns.last().and_then(|f| f.func) else {
            self.err("lambdas are not allowed in global initializers", span);
            return self.invalid(span);
        };
        let ptys: Vec<TypeId> = params.iter().map(|p| self.resolve_type(&p.ty)).collect();
        let ret_ty = ret.map(|r| self.resolve_type(r));
        let fid = FuncId(self.funcs.len() as u32);
        self.lambda_count += 1;
        let name = format!("lambda{}", self.lambda_count);
        let mut f = self.dummy_function(name.clone(), hir::FuncKind::Lambda { parent }, self.funcs[parent.0 as usize].namespace);
        f.full_name = format!("{}${name}", self.funcs[parent.0 as usize].full_name);
        f.symbol = format!("slang_{}", f.full_name.replace('.', "__").replace('$', "_"));
        f.span = span;
        self.funcs.push(f);
        self.func_src.push(FuncSrc::Synthetic);
        self.sigs.push(Sig { params: ptys.clone(), ret: ret_ty.unwrap_or(self.tt.void), is_async: false });
        self.fns.push(FnCtx::new(Some(fid), ret_ty.unwrap_or(self.tt.void), false, "lambda".into()));
        for (p, t) in params.iter().zip(&ptys) {
            self.declare_local(&p.name.name, *t, LocalKind::Param, p.span);
        }
        let (block, ret) = match body {
            ast::LambdaBody::Block(b) => {
                let r = ret_ty.unwrap_or(self.tt.void);
                (self.fn_body(b, r, span), r)
            }
            ast::LambdaBody::Expr(x) => {
                let v = match ret_ty {
                    Some(t) => self.expr_to(x, t, Ctx::Return),
                    None => {
                        let v = self.expr(x, None);
                        let v = if matches!(self.tt.get(v.ty), Type::Void) { v } else { self.require_value(v) };
                        self.default_type(v)
                    }
                };
                let r = v.ty;
                let sp = v.span;
                let stmt = if matches!(self.tt.get(r), Type::Void) {
                    hir::StmtKind::Expr(v)
                } else {
                    hir::StmtKind::Return(hir::ReturnValue::Value(v))
                };
                (hir::Block { stmts: vec![hir::Stmt { kind: stmt, span: sp }], span: sp }, r)
            }
        };
        let ctx = self.fns.pop().unwrap();
        let captures: Vec<hir::CaptureSource> = ctx.captures.iter().map(|c| c.source).collect();
        let f = &mut self.funcs[fid.0 as usize];
        f.params = (0..params.len()).map(|i| LocalId(i as u32)).collect();
        f.locals = ctx.locals;
        f.captures = ctx.captures;
        f.body = block;
        f.ret = ret;
        self.sigs[fid.0 as usize].ret = ret;
        let ty = self.tt.func(ptys, ret);
        mk(ExprKind::Closure { func: fid, captures }, ty, span)
    }
}

/// Removes `Paren` wrappers.
pub(crate) fn strip_parens(e: &ast::Expr) -> &ast::Expr {
    match &e.kind {
        ast::ExprKind::Paren(x) => strip_parens(x),
        _ => e,
    }
}

fn arith(op: ast::BinaryOp) -> BinOp {
    use ast::BinaryOp as B;
    match op {
        B::Add => BinOp::Add,
        B::Sub => BinOp::Sub,
        B::Mul => BinOp::Mul,
        B::Div => BinOp::Div,
        B::Rem => BinOp::Rem,
        B::BitAnd => BinOp::BitAnd,
        B::BitOr => BinOp::BitOr,
        B::BitXor => BinOp::BitXor,
        B::Shl => BinOp::Shl,
        B::Shr => BinOp::Shr,
        B::Eq => BinOp::Eq,
        B::Ne => BinOp::Ne,
        B::Lt => BinOp::Lt,
        B::Le => BinOp::Le,
        B::Gt => BinOp::Gt,
        B::Ge => BinOp::Ge,
        B::And => BinOp::And,
        B::Or => BinOp::Or,
    }
}

/// Source spelling of an operator.
pub(crate) fn op_symbol(op: ast::BinaryOp) -> &'static str {
    use ast::BinaryOp as B;
    match op {
        B::Add => "+",
        B::Sub => "-",
        B::Mul => "*",
        B::Div => "/",
        B::Rem => "%",
        B::BitAnd => "&",
        B::BitOr => "|",
        B::BitXor => "^",
        B::Shl => "<<",
        B::Shr => ">>",
        B::Eq => "==",
        B::Ne => "!=",
        B::Lt => "<",
        B::Le => "<=",
        B::Gt => ">",
        B::Ge => ">=",
        B::And => "&&",
        B::Or => "||",
    }
}
