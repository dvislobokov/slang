//! Operator overloading (DESIGN.md "Перегрузка операторов").
//!
//! Operators on struct and enum values lower to direct calls of fixed
//! methods; there is no dynamic dispatch and no user-defined operator:
//!
//! | operator | method |
//! |---|---|
//! | `a + b`, `-`, `*`, `/`, `%`, `@` | `a.Add(b)`, `Sub`, `Mul`, `Div`, `Rem`, `MatMul` |
//! | `-a` | `a.Neg()` |
//! | `a[i]` / `a[i] = v` | `a.Index(i)` / `a.IndexSet(i, v)` |
//! | `a == b` / `a != b` | `a.Equals(b)` / `!a.Equals(b)` (only if the method exists) |
//! | `a < b` (and `<= > >=`) | `a.Compare(b) < 0` (only if the method exists) |
//! | `a op= b` | `a = a op b` (`a[i] op= b`: `a.IndexSet(i, a.Index(i) op b)`) |
//!
//! The **left** operand selects the method. The right operand is checked
//! against the method's parameter type, so `func Mul(k float) Vec` makes
//! `v * 2.5` valid while `2.5 * v` is an error. The result type is the
//! method's result type (`Time.Sub(Time) Duration`, or `T!` for a fallible
//! method such as `Decimal.Div`). Inside generic code, the builtin
//! constraints `Add`, `Sub`, `Mul`, `Div`, `Rem`, `Neg` and `MatMul` allow
//! the operator on `T` (see [`Sema::op_bound_ok`]).

use super::coerce::Ctx;
use super::generics::Bound;
use super::*;
use crate::hir::{BinOp, Callee, ExprKind, Stmt, StmtKind, UnOp};

/// Builtin operator constraints (`T: Add`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum OpIface {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Neg,
    MatMul,
}

impl OpIface {
    pub(crate) fn from_name(n: &str) -> Option<OpIface> {
        Some(match n {
            "Add" => OpIface::Add,
            "Sub" => OpIface::Sub,
            "Mul" => OpIface::Mul,
            "Div" => OpIface::Div,
            "Rem" => OpIface::Rem,
            "Neg" => OpIface::Neg,
            "MatMul" => OpIface::MatMul,
            _ => return None,
        })
    }

    pub(crate) fn name(self) -> &'static str {
        match self {
            OpIface::Add => "Add",
            OpIface::Sub => "Sub",
            OpIface::Mul => "Mul",
            OpIface::Div => "Div",
            OpIface::Rem => "Rem",
            OpIface::Neg => "Neg",
            OpIface::MatMul => "MatMul",
        }
    }

    fn of_binary(op: ast::BinaryOp) -> Option<OpIface> {
        use ast::BinaryOp as B;
        Some(match op {
            B::Add => OpIface::Add,
            B::Sub => OpIface::Sub,
            B::Mul => OpIface::Mul,
            B::Div => OpIface::Div,
            B::Rem => OpIface::Rem,
            B::MatMul => OpIface::MatMul,
            _ => return None,
        })
    }
}

/// Method implementing an operator for a binary operator, if it can be
/// overloaded at all.
pub(crate) fn binary_method(op: ast::BinaryOp) -> Option<&'static str> {
    use ast::BinaryOp as B;
    match op {
        B::Eq | B::Ne => Some("Equals"),
        B::Lt | B::Le | B::Gt | B::Ge => Some("Compare"),
        _ => OpIface::of_binary(op).map(OpIface::name),
    }
}

/// A resolved operator method.
#[derive(Clone, Debug)]
pub(crate) struct OpMethod {
    pub fid: FuncId,
    /// Embedded-field path for a promoted method (struct receivers).
    pub path: Vec<u32>,
    pub params: Vec<TypeId>,
    pub ret: TypeId,
    pub is_async: bool,
    /// The receiver is an enum value (passed by value).
    pub is_enum: bool,
}

/// Right operand of an overloaded operator: not yet checked (so that it
/// takes the method's parameter type) or already checked.
pub(crate) enum OpArg<'e> {
    Ast(&'e ast::Expr),
    Hir(hir::Expr),
}

impl<'a> Sema<'a> {
    /// Types whose operators come from methods: struct and enum values
    /// (and, with `allow_ptr`, pointers to structs: indexing only, since
    /// `==` on pointers compares addresses).
    pub(crate) fn op_overload_type(&self, ty: TypeId, allow_ptr: bool) -> bool {
        match self.tt.get(ty) {
            Type::Struct(_) | Type::Enum(_) => true,
            Type::Pointer(t) if allow_ptr => matches!(self.tt.get(*t), Type::Struct(_)),
            _ => false,
        }
    }

    /// Non-static method `name` of a struct, pointer-to-struct or enum type.
    pub(crate) fn op_method(&self, ty: TypeId, name: &str) -> Option<OpMethod> {
        let t = match self.tt.get(ty) {
            Type::Pointer(inner) => *inner,
            _ => ty,
        };
        let (fid, path, is_enum) = match self.tt.get(t) {
            Type::Struct(d) => {
                let (fid, path) = self.find_method(d.0, name)?;
                (fid, path, false)
            }
            Type::Enum(d) => (*self.enum_methods.get(&(d.0, name.to_string()))?, Vec::new(), true),
            _ => return None,
        };
        let sig = &self.sigs[fid.0 as usize];
        Some(OpMethod { fid, path, params: sig.params.clone(), ret: sig.ret, is_async: sig.is_async, is_enum })
    }

    /// True if a type parameter's interface constraint declares `name`.
    fn param_has_method(&self, ty: TypeId, name: &str) -> bool {
        self.param_methods(ty).is_some_and(|ms| ms.iter().any(|m| m.name == name))
    }

    /// The operator method `name` of `ty` with `nparams` parameters,
    /// reporting a missing or misshapen method (`sym` names the operator).
    fn op_lookup(&mut self, ty: TypeId, sym: &str, name: &str, nparams: usize, span: Span) -> Option<OpMethod> {
        let t = self.tn(ty);
        let shape = op_shape(name, &t);
        let Some(m) = self.op_method(ty, name) else {
            self.err_help(format!("operator {sym} is not defined on {t}: {t} has no method {name}"), span, format!("declare `{shape}` in {t} to overload {sym}"));
            return None;
        };
        let ret = m.ret;
        let bad_ret = match name {
            "Equals" => ret != self.tt.bool_,
            "Compare" => ret != self.tt.int,
            "IndexSet" => false,
            _ => matches!(self.tt.get(ret), Type::Void),
        };
        if m.is_async || m.params.len() != nparams || bad_ret {
            let found = format!("func {name}{}", &self.sig_string(&m.params, ret)[4..]);
            let found = if m.is_async { format!("async {found}") } else { found };
            self.err_help(format!("operator {sym} on {t} needs method {name} of the form `{shape}`, found `{found}`"), span, format!("change the method to `{shape}` (operator methods are not async)"));
            return None;
        }
        Some(m)
    }

    /// The receiver argument of method `m` called on `base`.
    fn op_recv(&mut self, base: hir::Expr, m: &OpMethod) -> hir::Expr {
        if m.is_enum {
            return base;
        }
        if m.path.is_empty() && matches!(self.tt.get(base.ty), Type::Pointer(_)) {
            return base;
        }
        let mut b = base;
        for &idx in &m.path {
            let d = self.auto_deref(b);
            let Type::Struct(sd) = *self.tt.get(d.ty) else { unreachable!("promoted method through a non-struct") };
            let fty = self.structs[sd.0 as usize].fields[idx as usize].ty;
            let sp = d.span;
            b = mk(ExprKind::Field { base: Box::new(d), index: idx }, fty, sp);
        }
        if matches!(self.tt.get(b.ty), Type::Pointer(_)) {
            b
        } else {
            self.addr_of(b)
        }
    }

    /// Direct call of operator method `m` on `base` with checked `args`.
    pub(crate) fn op_call(&mut self, base: hir::Expr, m: &OpMethod, args: Vec<hir::Expr>, span: Span) -> hir::Expr {
        let recv = self.op_recv(base, m);
        let mut hargs = vec![recv];
        hargs.extend(args);
        mk(ExprKind::Call { callee: Callee::Func(m.fid), args: hargs }, m.ret, span)
    }

    fn op_arg(&mut self, arg: OpArg<'_>, ty: TypeId, what: String) -> hir::Expr {
        match arg {
            OpArg::Ast(e) => self.expr_to(e, ty, Ctx::Arg(0, what)),
            OpArg::Hir(h) => self.coerce(h, ty, Ctx::Arg(0, what)),
        }
    }

    /// Whether `l op r` with a checked left operand `lv` is overloaded
    /// (`r` is the unchecked right operand, when known).
    pub(crate) fn binary_overloaded(&self, op: ast::BinaryOp, lty: TypeId, r: Option<&ast::Expr>) -> bool {
        let Some(name) = binary_method(op) else { return false };
        if r.is_some_and(|r| matches!(expr::strip_parens(r).kind, ast::ExprKind::Nil)) {
            return false;
        }
        if matches!(self.tt.get(lty), Type::Param(_)) {
            return self.param_has_method(lty, name);
        }
        if !self.op_overload_type(lty, false) {
            return false;
        }
        // `==` and ordering fall back to the builtin rules without the method.
        match op {
            ast::BinaryOp::Eq | ast::BinaryOp::Ne | ast::BinaryOp::Lt | ast::BinaryOp::Le | ast::BinaryOp::Gt | ast::BinaryOp::Ge => {
                self.op_method(lty, name).is_some()
            }
            _ => true,
        }
    }

    /// `lv op r` through the operator method of `lv`'s type.
    pub(crate) fn op_binary(&mut self, op: ast::BinaryOp, lv: hir::Expr, r: OpArg<'_>, span: Span) -> hir::Expr {
        use ast::BinaryOp as B;
        let name = binary_method(op).expect("overloadable operator");
        let sym = expr::op_symbol(op);
        let call = if matches!(self.tt.get(lv.ty), Type::Param(_)) {
            // Interface-constrained type parameter: a method call (only
            // reached in the abstract check of a generic body).
            let OpArg::Ast(re) = r else {
                return self.invalid(span);
            };
            let id = ast::Ident { name: name.to_string(), span };
            self.method_call(lv, &id, std::slice::from_ref(re), span)
        } else {
            let lty = lv.ty;
            let Some(m) = self.op_lookup(lty, sym, name, 1, span) else {
                if let OpArg::Ast(re) = r {
                    self.value(re, None);
                }
                return self.invalid(span);
            };
            let what = format!("{}.{name} (operator {sym})", self.tn(lty));
            let arg = self.op_arg(r, m.params[0], what);
            self.op_call(lv, &m, vec![arg], span)
        };
        if self.tt.is_invalid(call.ty) {
            return call;
        }
        let bool_ = self.tt.bool_;
        match op {
            B::Ne => mk(ExprKind::Unary { op: UnOp::Not, operand: Box::new(call) }, bool_, span),
            B::Lt | B::Le | B::Gt | B::Ge => {
                let hop = match op {
                    B::Lt => BinOp::Lt,
                    B::Le => BinOp::Le,
                    B::Gt => BinOp::Gt,
                    _ => BinOp::Ge,
                };
                let zero = mk(ExprKind::Const(hir::ConstValue::Int(0)), call.ty, span);
                mk(ExprKind::Binary { op: hop, lhs: Box::new(call), rhs: Box::new(zero) }, bool_, span)
            }
            _ => call,
        }
    }

    /// `-v` on a struct or enum value: `v.Neg()`.
    pub(crate) fn op_neg(&mut self, v: hir::Expr, span: Span) -> hir::Expr {
        if matches!(self.tt.get(v.ty), Type::Param(_)) {
            let id = ast::Ident { name: "Neg".into(), span };
            return self.method_call(v, &id, &[], span);
        }
        let ty = v.ty;
        match self.op_lookup(ty, "-", "Neg", 0, span) {
            Some(m) => self.op_call(v, &m, Vec::new(), span),
            None => self.invalid(span),
        }
    }

    /// Whether `-v` on a value of type `ty` is overloaded.
    pub(crate) fn neg_overloaded(&self, ty: TypeId) -> bool {
        self.op_overload_type(ty, false) || (matches!(self.tt.get(ty), Type::Param(_)) && self.param_has_method(ty, "Neg"))
    }

    /// Whether `b[i]` on a value of type `ty` is overloaded (`name` is
    /// `Index` or `IndexSet`).
    pub(crate) fn index_overloaded(&self, ty: TypeId, name: &str) -> bool {
        self.op_overload_type(ty, true) || (matches!(self.tt.get(ty), Type::Param(_)) && self.param_has_method(ty, name))
    }

    /// `b[i]` on a struct value: `b.Index(i)`.
    pub(crate) fn op_index(&mut self, b: hir::Expr, index: &ast::Expr, span: Span) -> hir::Expr {
        if let ast::ExprKind::Range(..) = index.kind {
            self.err_help(format!("cannot slice {}: slicing cannot be overloaded", self.tn(b.ty)), span, "call a method that returns the part you need");
            return self.invalid(span);
        }
        if matches!(self.tt.get(b.ty), Type::Param(_)) {
            let id = ast::Ident { name: "Index".into(), span };
            return self.method_call(b, &id, std::slice::from_ref(index), span);
        }
        let ty = b.ty;
        let Some(m) = self.op_lookup(ty, "[]", "Index", 1, span) else {
            self.value(index, None);
            return self.invalid(span);
        };
        let what = format!("{}.Index (operator [])", self.tn(ty));
        let i = self.op_arg(OpArg::Ast(index), m.params[0], what);
        self.op_call(b, &m, vec![i], span)
    }

    /// `b[i] = v` / `b[i] op= v` on a struct value (`b` checked): calls
    /// `IndexSet` (with `Index` for the compound form).
    pub(crate) fn op_index_assign(&mut self, b: hir::Expr, index: &ast::Expr, op: Option<ast::BinaryOp>, value: &ast::Expr, span: Span) -> StmtKind {
        let ty = b.ty;
        if let ast::ExprKind::Range(..) = index.kind {
            self.err(format!("cannot assign to a slice of {}: slicing cannot be overloaded", self.tn(ty)), span);
            return StmtKind::Block(hir::Block::default());
        }
        if matches!(self.tt.get(ty), Type::Param(_)) {
            // Abstract check of a generic body: typed as method calls.
            let id = ast::Ident { name: "IndexSet".into(), span };
            let call = match op {
                None => self.method_call(b, &id, &[index.clone(), value.clone()], span),
                Some(_) => {
                    self.value(value, None);
                    self.invalid(span)
                }
            };
            return StmtKind::Expr(call);
        }
        let Some(set) = self.op_lookup(ty, "[]=", "IndexSet", 2, span) else {
            self.value(index, None);
            self.value(value, None);
            return StmtKind::Block(hir::Block::default());
        };
        if !matches!(self.tt.get(set.ret), Type::Void) {
            let t = self.tn(ty);
            self.err_help(format!("operator []= on {t} needs IndexSet to return nothing, found {}", self.tn(set.ret)), span, format!("declare `{}`", op_shape("IndexSet", &t)));
            return StmtKind::Block(hir::Block::default());
        }
        let tname = self.tn(ty);
        let i = self.op_arg(OpArg::Ast(index), set.params[0], format!("{tname}.IndexSet (operator []=)"));
        let Some(bop) = op else {
            let v = self.op_arg(OpArg::Ast(value), set.params[1], format!("{tname}.IndexSet (operator []=)"));
            return StmtKind::Expr(self.op_call(b, &set, vec![i, v], span));
        };
        // `b[i] op= v`: the receiver is read twice, the index once.
        if !simple_place(&b) {
            self.err_help(format!("compound assignment through {tname}.IndexSet needs a variable or field on the left"), span, "store the value in a variable first");
            self.value(value, None);
            return StmtKind::Block(hir::Block::default());
        }
        let Some(get) = self.op_lookup(ty, "[]", "Index", 1, span) else {
            self.value(value, None);
            return StmtKind::Block(hir::Block::default());
        };
        let mut stmts = Vec::new();
        let i = if matches!(i.kind, ExprKind::Local(_) | ExprKind::Const(_)) {
            i
        } else {
            let t = self.temp(i.ty, i.span);
            let (ity, isp) = (i.ty, i.span);
            stmts.push(Stmt { kind: StmtKind::Let { local: t, init: Some(i) }, span: isp });
            mk(ExprKind::Local(t), ity, isp)
        };
        let gi = self.coerce(i.clone(), get.params[0], Ctx::Other);
        let cur = self.op_call(b.clone(), &get, vec![gi], span);
        let new = self.compound_value(bop, cur, value, span);
        let new = self.coerce(new, set.params[1], Ctx::Assign);
        stmts.push(Stmt { kind: StmtKind::Expr(self.op_call(b, &set, vec![i, new], span)), span });
        StmtKind::Block(hir::Block { stmts, span })
    }

    /// The new value of `cur op= value` (`cur` checked).
    pub(crate) fn compound_value(&mut self, bop: ast::BinaryOp, cur: hir::Expr, value: &ast::Expr, span: Span) -> hir::Expr {
        if self.binary_overloaded(bop, cur.ty, Some(value)) {
            let v = self.op_binary(bop, cur, OpArg::Ast(value), span);
            return self.require_value(v);
        }
        let rv = self.value(value, Some(cur.ty));
        self.make_binary(bop, cur, rv, span)
    }

    /// `t op= v` where `t` (a checked place) has an overloadable type.
    pub(crate) fn op_compound_assign(&mut self, bop: ast::BinaryOp, t: hir::Expr, value: &ast::Expr, span: Span) -> StmtKind {
        // `xs[i + 1] op= v` / `m[key()] op= v`: the index (key) is
        // evaluated once into a temporary, the place is then read and
        // written through it.
        let mut stmts = Vec::new();
        let t = match t.kind {
            ExprKind::Index { base, index } if simple_place(&base) && !matches!(index.kind, ExprKind::Local(_) | ExprKind::Const(_)) => {
                let (ity, isp) = (index.ty, index.span);
                let tmp = self.temp(ity, isp);
                stmts.push(Stmt { kind: StmtKind::Let { local: tmp, init: Some(*index) }, span: isp });
                mk(ExprKind::Index { base, index: Box::new(mk(ExprKind::Local(tmp), ity, isp)) }, t.ty, t.span)
            }
            ExprKind::MapGet { map, key } if simple_place(&map) && !matches!(key.kind, ExprKind::Local(_) | ExprKind::Const(_)) => {
                let (kty, ksp) = (key.ty, key.span);
                let tmp = self.temp(kty, ksp);
                stmts.push(Stmt { kind: StmtKind::Let { local: tmp, init: Some(*key) }, span: ksp });
                mk(ExprKind::MapGet { map, key: Box::new(mk(ExprKind::Local(tmp), kty, ksp)) }, t.ty, t.span)
            }
            kind => hir::Expr { kind, ..t },
        };
        if !simple_place(&t) {
            self.err_help(format!("compound assignment with an overloaded operator {}= needs a variable or field on the left", expr::op_symbol(bop)), span, "store the value in a variable first");
            self.value(value, None);
            return StmtKind::Block(hir::Block::default());
        }
        let ty = t.ty;
        let new = self.compound_value(bop, t.clone(), value, span);
        let new = self.coerce(new, ty, Ctx::Assign);
        if stmts.is_empty() {
            return StmtKind::Assign { target: t, value: new };
        }
        stmts.push(Stmt { kind: StmtKind::Assign { target: t, value: new }, span });
        StmtKind::Block(hir::Block { stmts, span })
    }

    // -- constraints -----------------------------------------------------------

    /// Does `ty` satisfy the builtin operator constraint `k`: a numeric
    /// type with the operator, or a type with `func K(other T) T`?
    pub(crate) fn op_bound_ok(&self, ty: TypeId, k: OpIface) -> Result<(), String> {
        let t = self.tn(ty);
        if let Type::Param(p) = self.tt.get(ty) {
            return if self.gen.tparams[*p as usize].bound == Bound::Op(k) {
                Ok(())
            } else {
                Err(format!("type parameter {t} does not satisfy constraint {} (constrain it: {t}: {})", k.name(), k.name()))
            };
        }
        let builtin = match k {
            OpIface::Add => self.tt.is_numeric(ty) || matches!(self.tt.get(ty), Type::String),
            OpIface::Sub | OpIface::Mul | OpIface::Div | OpIface::Neg => self.tt.is_numeric(ty),
            OpIface::Rem => self.tt.is_integer(ty),
            OpIface::MatMul => false,
        };
        if builtin || self.tt.is_invalid(ty) {
            return Ok(());
        }
        let name = k.name();
        let want = if k == OpIface::Neg { Vec::new() } else { vec![ty] };
        match self.op_method(ty, name) {
            Some(m) if m.params == want && m.ret == ty && !m.is_async => Ok(()),
            Some(m) => Err(format!("type {t} does not satisfy constraint {name}: method {name} is `func {name}{}`, expected `{}`", &self.sig_string(&m.params, m.ret)[4..], op_shape(name, &t))),
            None => Err(format!("type {t} does not satisfy constraint {name}: missing method {name} (`{}`)", op_shape(name, &t))),
        }
    }

    /// `func Equals(other T) bool` / `func Compare(other T) int` of `ty`.
    pub(crate) fn has_cmp_method(&self, ty: TypeId, name: &str) -> bool {
        let want = if name == "Equals" { self.tt.bool_ } else { self.tt.int };
        self.op_overload_type(ty, false) && self.op_method(ty, name).is_some_and(|m| m.params == [ty] && m.ret == want && !m.is_async)
    }
}

/// The expected form of operator method `name` on type `t`.
fn op_shape(name: &str, t: &str) -> String {
    match name {
        "Neg" => format!("func Neg() {t}"),
        "Equals" => format!("func Equals(other {t}) bool"),
        "Compare" => format!("func Compare(other {t}) int"),
        "Index" => "func Index(i I) V".to_string(),
        "IndexSet" => "func IndexSet(i I, v V)".to_string(),
        _ => format!("func {name}(other {t}) {t}"),
    }
}

/// A place that can be evaluated twice without side effects: variables,
/// `this`, globals and fields/dereferences of those.
pub(crate) fn simple_place(e: &hir::Expr) -> bool {
    match &e.kind {
        ExprKind::Local(_) | ExprKind::Upvalue(_) | ExprKind::Global(_) => true,
        ExprKind::Field { base, .. } | ExprKind::Deref(base) => simple_place(base),
        ExprKind::Index { base, index } => simple_place(base) && matches!(index.kind, ExprKind::Local(_) | ExprKind::Const(_)),
        ExprKind::MapGet { map, key } => simple_place(map) && matches!(key.kind, ExprKind::Local(_) | ExprKind::Const(_)),
        _ => false,
    }
}
