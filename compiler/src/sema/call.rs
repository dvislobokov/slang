//! Calls: user functions, methods (with implicit `&` receivers and
//! interface dispatch), closures, enum variant constructors, builtins,
//! conversions and the special stdlib functions.

use super::coerce::Ctx;
use super::resolve::PathRes;
use super::stdlib::Special;
use super::*;
use crate::hir::{Builtin, Callee, ConvKind, ExprKind, PrintItem, PrintOp};

impl<'a> Sema<'a> {
    /// `callee(args)`.
    pub(crate) fn call(&mut self, callee: &ast::Expr, args: &[ast::Expr], span: Span, expected: Option<TypeId>) -> hir::Expr {
        let target = expr::strip_parens(callee);
        if let Some(segs) = Self::flatten_path(target) {
            let last = *segs.last().unwrap();
            let head_local = self.lookup_local(&segs[0].name, false).is_some();
            if head_local {
                if segs.len() == 1 {
                    let f = self.path_expr(&segs, callee.span, None);
                    return self.call_value(f, args, span);
                }
                let base = self.path_expr(&segs[..segs.len() - 1], callee.span, None);
                return self.method_call(base, last, args, span);
            }
            let Some((res, n)) = self.resolve_segments(&segs, true) else {
                if self.lookup_global(segs[0], false).is_none() && !self.diags.has_errors_at(segs[0].span, self.cur_file) {
                    self.err(format!("unknown identifier {}", segs[0].name), segs[0].span);
                }
                self.check_args_loose(args);
                return self.invalid(span);
            };
            if n == segs.len() {
                return self.call_res(res, last, args, span, expected);
            }
            let sp = segs[0].span.to(segs[n - 1].span);
            let mut base = self.path_value(res, &segs[n - 1].name, sp, None);
            for seg in &segs[n..segs.len() - 1] {
                base = self.field_access(base, seg);
            }
            return self.method_call(base, last, args, span);
        }
        if let ast::ExprKind::Field { base, name } = &target.kind {
            let b = self.value(base, None);
            return self.method_call(b, name, args, span);
        }
        let f = self.value(callee, None);
        self.call_value(f, args, span)
    }

    /// Checks arguments without a signature (after an earlier error).
    fn check_args_loose(&mut self, args: &[ast::Expr]) {
        for a in args {
            self.expr(a, None);
        }
    }

    /// Checks call arguments against parameter types.
    pub(crate) fn check_args(&mut self, params: &[TypeId], args: &[ast::Expr], fname: &str, span: Span) -> Vec<hir::Expr> {
        if params.len() != args.len() {
            self.err(format!("wrong number of arguments in call to {fname}: expected {}, got {}", params.len(), args.len()), span);
            self.check_args_loose(args);
            return Vec::new();
        }
        args.iter().zip(params).enumerate().map(|(i, (a, p))| self.expr_to(a, *p, Ctx::Arg(i, fname.to_string()))).collect()
    }

    /// Call of a symbolically resolved callee.
    fn call_res(&mut self, res: PathRes, name: &ast::Ident, args: &[ast::Expr], span: Span, expected: Option<TypeId>) -> hir::Expr {
        match res {
            PathRes::Member(Member::Func(f)) | PathRes::StaticMethod(f) => {
                let sig = self.sigs[f.0 as usize].clone();
                let fname = self.funcs[f.0 as usize].name.clone();
                let args = self.check_args(&sig.params, args, &fname, span);
                let ty = if sig.is_async { self.tt.task(sig.ret) } else { sig.ret };
                mk(ExprKind::Call { callee: Callee::Func(f), args }, ty, span)
            }
            PathRes::Member(Member::StdFunc(i)) => {
                let sf = stdlib::FUNCS[i];
                let (params, ret) = sf.signature(&mut self.tt);
                let args = self.check_args(&params, args, &sf.path(), span);
                let x = self.extern_id(sf.path(), sf.symbol, params, ret);
                mk(ExprKind::Call { callee: Callee::Extern(x), args }, ret, span)
            }
            PathRes::Member(Member::Special(sp)) => self.special(sp, args, span),
            PathRes::Builtin(b) => self.builtin(b, args, span, expected),
            PathRes::BuiltinType(t) => self.conversion(t, args, span),
            PathRes::Variant(e, v) => {
                let fields: Vec<TypeId> = self.enums[e as usize].variants[v as usize].fields.iter().map(|f| f.1).collect();
                let vname = format!("{}.{}", self.enums[e as usize].name, name.name);
                if fields.is_empty() {
                    self.err(format!("variant {vname} has no fields; write it without parentheses"), span);
                }
                let args = self.check_args(&fields, args, &vname, span);
                let ty = self.enum_ty(e);
                mk(ExprKind::EnumLit { enum_id: e, variant: v, args }, ty, span)
            }
            PathRes::Member(Member::Global(_)) => {
                let f = self.path_value(res, &name.name, name.span, None);
                self.call_value(f, args, span)
            }
            PathRes::Member(Member::StdConst(_)) => {
                self.err(format!("{} is a constant, not a function", name.name), name.span);
                self.invalid(span)
            }
            PathRes::Member(Member::Struct(_) | Member::Interface(_) | Member::Enum(_)) => {
                self.err(format!("cannot call type {} (use a struct literal {}{{...}} or a static constructor)", name.name, name.name), name.span);
                self.check_args_loose(args);
                self.invalid(span)
            }
            PathRes::Member(Member::Ns(_)) => {
                self.err(format!("{} is a namespace, not a function", name.name), name.span);
                self.invalid(span)
            }
        }
    }

    /// Call of a closure / function value.
    pub(crate) fn call_value(&mut self, f: hir::Expr, args: &[ast::Expr], span: Span) -> hir::Expr {
        match self.tt.get(f.ty).clone() {
            Type::Func(sig) => {
                let args = self.check_args(&sig.params, args, "function value", span);
                mk(ExprKind::Call { callee: Callee::Value(Box::new(f)), args }, sig.ret, span)
            }
            Type::Invalid => {
                self.check_args_loose(args);
                self.invalid(span)
            }
            _ => {
                self.err(format!("cannot call non-function value of type {}", self.tn(f.ty)), f.span);
                self.check_args_loose(args);
                self.invalid(span)
            }
        }
    }

    /// `base.name(args)`: struct methods (implicit `&`, promotion through
    /// embedded fields), func-typed fields, interface methods.
    pub(crate) fn method_call(&mut self, base: hir::Expr, name: &ast::Ident, args: &[ast::Expr], span: Span) -> hir::Expr {
        if self.tt.is_invalid(base.ty) {
            self.check_args_loose(args);
            return self.invalid(span);
        }
        if let Some(sid) = self.struct_of(base.ty) {
            if let Some((fid, path)) = self.find_method(sid, &name.name) {
                let owner = self.funcs[fid.0 as usize].namespace;
                self.check_member_visibility(owner, &name.name, name.span, true);
                let is_ptr = matches!(self.tt.get(base.ty), Type::Pointer(_));
                let recv = if path.is_empty() && is_ptr {
                    base
                } else {
                    let mut b = base;
                    for idx in path {
                        let d = self.auto_deref(b);
                        let Type::Struct(sd) = *self.tt.get(d.ty) else { unreachable!() };
                        let fty = self.structs[sd.0 as usize].fields[idx as usize].ty;
                        let sp = d.span;
                        b = mk(ExprKind::Field { base: Box::new(d), index: idx }, fty, sp);
                    }
                    if matches!(self.tt.get(b.ty), Type::Pointer(_)) {
                        b
                    } else {
                        self.addr_of(b)
                    }
                };
                let sig = self.sigs[fid.0 as usize].clone();
                let fname = format!("{}.{}", self.structs[sid as usize].name, name.name);
                let mut hargs = vec![recv];
                hargs.extend(self.check_args(&sig.params, args, &fname, span));
                let ty = if sig.is_async { self.tt.task(sig.ret) } else { sig.ret };
                return mk(ExprKind::Call { callee: Callee::Func(fid), args: hargs }, ty, span);
            }
            if self.find_field(sid, &name.name).is_some() {
                let f = self.field_access(base, name);
                return self.call_value(f, args, span);
            }
            let sname = self.structs[sid as usize].name.clone();
            self.err(format!("struct {sname} has no method {}", name.name), name.span);
            self.check_args_loose(args);
            return self.invalid(span);
        }
        if self.tt.is_interface(base.ty) {
            let methods = self.iface_methods(base.ty);
            let Some(idx) = methods.iter().position(|m| m.name == name.name) else {
                self.err(format!("interface {} has no method {}", self.tn(base.ty), name.name), name.span);
                self.check_args_loose(args);
                return self.invalid(span);
            };
            let m = methods[idx].clone();
            let fname = format!("{}.{}", self.tn(base.ty), m.name);
            let args = self.check_args(&m.params, args, &fname, span);
            return mk(ExprKind::Call { callee: Callee::Interface { recv: Box::new(base), method: idx as u32 }, args }, m.ret, span);
        }
        self.err(
            format!("cannot call method {} on value of type {} (methods exist only on structs and interfaces)", name.name, self.tn(base.ty)),
            name.span,
        );
        self.check_args_loose(args);
        self.invalid(span)
    }

    fn arity(&mut self, name: &str, args: &[ast::Expr], n: usize, span: Span) -> bool {
        if args.len() != n {
            self.err(format!("wrong number of arguments in call to {name}: expected {n}, got {}", args.len()), span);
            self.check_args_loose(args);
            return false;
        }
        true
    }

    fn builtin(&mut self, b: BuiltinFn, args: &[ast::Expr], span: Span, expected: Option<TypeId>) -> hir::Expr {
        let bi = |op, args, ty| mk(ExprKind::Builtin { op, args }, ty, span);
        match b {
            BuiltinFn::Error => {
                if !self.arity("error", args, 1, span) {
                    return self.invalid(span);
                }
                let m = self.expr_to(&args[0], self.tt.string, Ctx::Arg(0, "error".into()));
                bi(Builtin::ErrorNew, vec![m], self.tt.error)
            }
            BuiltinFn::Len => {
                if !self.arity("len", args, 1, span) {
                    return self.invalid(span);
                }
                let v = self.value(&args[0], None);
                let op = match self.tt.get(v.ty) {
                    Type::String => Builtin::LenString,
                    Type::Slice(_) => Builtin::LenSlice,
                    Type::Array(..) => Builtin::LenArray,
                    Type::Map(..) => Builtin::LenMap,
                    Type::Invalid => return self.invalid(span),
                    _ => {
                        self.err(format!("invalid argument for len: {} (want string, slice, array or map)", self.tn(v.ty)), v.span);
                        return self.invalid(span);
                    }
                };
                bi(op, vec![v], self.tt.int)
            }
            BuiltinFn::Append => {
                if args.len() < 2 {
                    self.err("append needs a slice and at least one value", span);
                    self.check_args_loose(args);
                    return self.invalid(span);
                }
                let s = self.value(&args[0], expected);
                let Type::Slice(el) = *self.tt.get(s.ty) else {
                    if !self.tt.is_invalid(s.ty) {
                        self.err(format!("first argument of append must be a slice, found {}", self.tn(s.ty)), s.span);
                    }
                    self.check_args_loose(&args[1..]);
                    return self.invalid(span);
                };
                let ty = s.ty;
                let mut hargs = vec![s];
                for (i, a) in args[1..].iter().enumerate() {
                    hargs.push(self.expr_to(a, el, Ctx::Arg(i + 1, "append".into())));
                }
                bi(Builtin::Append, hargs, ty)
            }
            BuiltinFn::Delete => {
                if !self.arity("delete", args, 2, span) {
                    return self.invalid(span);
                }
                let m = self.value(&args[0], None);
                let Type::Map(k, _) = *self.tt.get(m.ty) else {
                    if !self.tt.is_invalid(m.ty) {
                        self.err(format!("first argument of delete must be a map, found {}", self.tn(m.ty)), m.span);
                    }
                    return self.invalid(span);
                };
                let key = self.expr_to(&args[1], k, Ctx::Arg(1, "delete".into()));
                bi(Builtin::Delete, vec![m, key], self.tt.void)
            }
            BuiltinFn::Panic => {
                if !self.arity("panic", args, 1, span) {
                    return self.invalid(span);
                }
                let v = self.value(&args[0], Some(self.tt.string));
                let v = if v.ty == self.tt.string || v.ty == self.tt.error || self.tt.is_invalid(v.ty) {
                    v
                } else if self.implements(v.ty, self.tt.error) {
                    self.coerce(v, self.tt.error, Ctx::Other)
                } else {
                    self.err(format!("panic needs a string or an error, found {}", self.tn(v.ty)), v.span);
                    v
                };
                bi(Builtin::Panic, vec![v], self.tt.void)
            }
            BuiltinFn::Typeof => {
                self.err("typeof (reflection) is not supported yet", span);
                self.check_args_loose(args);
                self.invalid(span)
            }
        }
    }

    /// `T(x)` conversions between numeric types, chars, strings and enums.
    fn conversion(&mut self, t: TypeId, args: &[ast::Expr], span: Span) -> hir::Expr {
        let tname = self.tn(t);
        if !self.arity(&tname, args, 1, span) {
            return self.invalid(span);
        }
        let v = self.value(&args[0], Some(t));
        let v = self.default_type(v);
        if v.ty == t || self.tt.is_invalid(v.ty) {
            return mk(v.kind, t, span);
        }
        let (si, sf) = (self.tt.is_integer(v.ty), self.tt.is_float(v.ty));
        let (ti, tf) = (self.tt.is_integer(t), self.tt.is_float(t));
        let src = self.tt.get(v.ty).clone();
        let dst = self.tt.get(t).clone();
        let kind = if si && ti {
            Some(ConvKind::IntToInt)
        } else if si && tf {
            Some(ConvKind::IntToFloat)
        } else if sf && ti {
            Some(ConvKind::FloatToInt)
        } else if sf && tf {
            Some(ConvKind::FloatToFloat)
        } else {
            match (&src, &dst) {
                (Type::Enum(d), Type::Int(_)) if self.enums[d.0 as usize].is_plain => Some(ConvKind::EnumToInt),
                (Type::Char, Type::String) => Some(ConvKind::CharToString),
                (Type::Int(_), Type::Char) => Some(ConvKind::IntToChar),
                (Type::Char, Type::Int(_)) => Some(ConvKind::CharToInt),
                _ => None,
            }
        };
        match kind {
            Some(kind) => mk(ExprKind::Convert { kind, value: Box::new(v) }, t, span),
            None => {
                self.err(format!("cannot convert {} to {tname}", self.tn(v.ty)), span);
                self.invalid(span)
            }
        }
    }

    /// Whether values of `t` can be printed by `println` / formatted.
    fn printable(&self, t: TypeId) -> bool {
        match self.tt.get(t) {
            Type::Int(_) | Type::Float32 | Type::Float64 | Type::Bool | Type::Char | Type::String | Type::Error | Type::Enum(_) | Type::Invalid => true,
            Type::Slice(e) | Type::Array(e, _) => self.printable(*e),
            _ => false,
        }
    }

    /// Checks a variadic "any" argument for printing/formatting.
    fn print_arg(&mut self, a: &ast::Expr) -> hir::Expr {
        let v = self.value(a, None);
        let v = self.default_type(v);
        if !self.printable(v.ty) {
            self.err(format!("value of type {} cannot be printed (print its fields or convert it to a string)", self.tn(v.ty)), v.span);
        }
        v
    }

    fn stringify(&mut self, v: hir::Expr) -> hir::Expr {
        if v.ty == self.tt.string {
            return v;
        }
        let span = v.span;
        mk(ExprKind::ToString(Box::new(v)), self.tt.string, span)
    }

    fn special(&mut self, sp: Special, args: &[ast::Expr], span: Span) -> hir::Expr {
        match sp {
            Special::Println | Special::Print => {
                let mut items = Vec::new();
                for (i, a) in args.iter().enumerate() {
                    if i > 0 {
                        items.push(PrintItem { op: PrintOp::Space, arg: None });
                    }
                    let v = self.print_arg(a);
                    let op = match self.tt.get(v.ty) {
                        Type::Int(k) if k.signed() => PrintOp::Int,
                        Type::Int(_) => PrintOp::Uint,
                        Type::Float32 | Type::Float64 => PrintOp::Float,
                        Type::Bool => PrintOp::Bool,
                        Type::Char => PrintOp::Char,
                        _ => PrintOp::Str,
                    };
                    let v = if op == PrintOp::Str { self.stringify(v) } else { v };
                    items.push(PrintItem { op, arg: Some(v) });
                }
                if sp == Special::Println {
                    items.push(PrintItem { op: PrintOp::Newline, arg: None });
                }
                mk(ExprKind::Print(items), self.tt.void, span)
            }
            Special::Format => {
                if args.is_empty() {
                    self.err("fmt.Format needs a template string", span);
                    return self.invalid(span);
                }
                let tmpl = self.expr_to(&args[0], self.tt.string, Ctx::Arg(0, "fmt.Format".into()));
                let mut parts = Vec::new();
                for a in &args[1..] {
                    let v = self.print_arg(a);
                    parts.push(self.stringify(v));
                }
                let sl = self.tt.slice(self.tt.string);
                let list = mk(ExprKind::SliceLit(parts), sl, span);
                let x = self.extern_id("std.fmt.Format".into(), stdlib::FORMAT_SYMBOL, vec![self.tt.string, sl], self.tt.string);
                mk(ExprKind::Call { callee: Callee::Extern(x), args: vec![tmpl, list] }, self.tt.string, span)
            }
            Special::TaskAll => {
                if args.is_empty() {
                    self.err("task.All needs at least one task", span);
                    return self.invalid(span);
                }
                let mut vs = Vec::new();
                let mut inner = Vec::new();
                let mut fallible = false;
                for a in args {
                    let v = self.value(a, None);
                    match *self.tt.get(v.ty) {
                        Type::Task(t) => match self.tt.fallible_inner(t) {
                            Some(u) => {
                                fallible = true;
                                inner.push(u);
                            }
                            None => inner.push(t),
                        },
                        Type::Invalid => inner.push(v.ty),
                        _ => {
                            self.err(format!("task.All arguments must be tasks, found {}", self.tn(v.ty)), v.span);
                            inner.push(self.tt.invalid);
                        }
                    }
                    vs.push(v);
                }
                let tup = self.tt.tuple(inner);
                let res = if fallible { self.tt.fallible(tup) } else { tup };
                let ty = self.tt.task(res);
                mk(ExprKind::TaskAll(vs), ty, span)
            }
            Special::ErrorsNew => {
                if !self.arity("errors.New", args, 1, span) {
                    return self.invalid(span);
                }
                let m = self.expr_to(&args[0], self.tt.string, Ctx::Arg(0, "errors.New".into()));
                mk(ExprKind::Builtin { op: Builtin::ErrorNew, args: vec![m] }, self.tt.error, span)
            }
            Special::ErrorsAs => {
                if !self.arity("errors.As", args, 2, span) {
                    return self.invalid(span);
                }
                let err = self.expr_to(&args[0], self.tt.error, Ctx::Arg(0, "errors.As".into()));
                let target = self.value(&args[1], None);
                let ok = match *self.tt.get(target.ty) {
                    Type::Pointer(p) => matches!(self.tt.get(p), Type::Struct(_)) && self.implements(p, self.tt.error),
                    Type::Invalid => true,
                    _ => false,
                };
                if !ok {
                    self.err(format!("errors.As target must be a pointer to a struct implementing error, found {}", self.tn(target.ty)), target.span);
                }
                mk(ExprKind::Builtin { op: Builtin::ErrorsAs, args: vec![err, target] }, self.tt.bool_, span)
            }
        }
    }
}
