//! Assignability, implicit conversions and structural interface checks.

use super::*;
use crate::hir::{ExprKind, MethodSig};

/// Where a value is being converted, for diagnostics.
#[derive(Clone, Debug)]
pub(crate) enum Ctx {
    Assign,
    Init,
    Arg(usize, String),
    Return,
    Field(String),
    Elem,
    Other,
}

impl<'a> Sema<'a> {
    /// Method set required by an interface-like type.
    pub(crate) fn iface_methods(&self, t: TypeId) -> Vec<MethodSig> {
        match self.tt.get(t) {
            Type::Interface(d) => self.interfaces[d.0 as usize].methods.clone(),
            Type::Error => vec![MethodSig { name: "Message".into(), params: Vec::new(), ret: self.tt.string }],
            _ => Vec::new(),
        }
    }

    /// Struct id of `S` or `*S`.
    pub(crate) fn struct_of(&self, t: TypeId) -> Option<u32> {
        match self.tt.get(t) {
            Type::Struct(d) => Some(d.0),
            Type::Pointer(p) => match self.tt.get(*p) {
                Type::Struct(d) => Some(d.0),
                _ => None,
            },
            _ => None,
        }
    }

    /// Checks that `src` (a struct or pointer to struct) has every method of
    /// interface `iface`; returns the problems found.
    pub(crate) fn missing_methods(&self, src: TypeId, iface: TypeId) -> Vec<String> {
        let Some(sid) = self.struct_of(src) else {
            return vec!["(not a struct)".into()];
        };
        let mut problems = Vec::new();
        for m in self.iface_methods(iface) {
            match self.find_method(sid, &m.name) {
                None => problems.push(format!("missing method {}", m.name)),
                Some((f, _)) => {
                    let sig = &self.sigs[f.0 as usize];
                    if sig.params != m.params || sig.ret != m.ret || sig.is_async {
                        let have = self.sig_string(&sig.params, sig.ret);
                        let want = self.sig_string(&m.params, m.ret);
                        problems.push(format!("method {} has type {have}, want {want}", m.name));
                    }
                }
            }
        }
        problems
    }

    /// `func(a, b) r` spelling of a signature.
    pub(crate) fn sig_string(&self, params: &[TypeId], ret: TypeId) -> String {
        let ps: Vec<String> = params.iter().map(|p| self.tn(*p)).collect();
        if matches!(self.tt.get(ret), Type::Void) {
            format!("func({})", ps.join(", "))
        } else {
            format!("func({}) {}", ps.join(", "), self.tn(ret))
        }
    }

    /// True if `src` can be stored in interface-like `target`.
    pub(crate) fn implements(&self, src: TypeId, target: TypeId) -> bool {
        if matches!(self.tt.get(target), Type::Any) {
            return true;
        }
        (self.struct_of(src).is_some() && self.missing_methods(src, target).is_empty()) || self.param_implements(src, target) || self.iface_subset(src, target)
    }

    /// Interface `src` has every method of interface `target` (same
    /// signatures): its values convert implicitly (decision 18).
    pub(crate) fn iface_subset(&self, src: TypeId, target: TypeId) -> bool {
        if src == target || !matches!(self.tt.get(src), Type::Interface(_)) || !matches!(self.tt.get(target), Type::Interface(_)) {
            return false;
        }
        let have = self.iface_methods(src);
        self.iface_methods(target).iter().all(|m| have.iter().any(|h| h.name == m.name && h.params == m.params && h.ret == m.ret))
    }

    /// Converts `e` to `target` if assignable (identity, typed `nil`,
    /// interface boxing), else reports a mismatch.
    pub(crate) fn coerce(&mut self, e: hir::Expr, target: TypeId, ctx: Ctx) -> hir::Expr {
        let src = e.ty;
        if src == target || self.tt.is_invalid(src) || self.tt.is_invalid(target) {
            return e;
        }
        if matches!(self.tt.get(src), Type::Nil) {
            if self.tt.is_nilable(target) {
                return mk(ExprKind::Nil, target, e.span);
            }
            let t = self.tn(target);
            self.err(format!("cannot use nil as {t}: {t} is not nilable (only pointers, slices, maps, functions and interfaces can be nil)"), e.span);
            return mk(ExprKind::Nil, self.tt.invalid, e.span);
        }
        if let (Type::Pointer(a), Type::Weak(b)) = (self.tt.get(src).clone(), self.tt.get(target).clone()) {
            if a == b {
                let span = e.span;
                return mk(ExprKind::Convert { kind: crate::hir::ConvKind::PtrToWeak, value: Box::new(e) }, target, span);
            }
        }
        if self.tt.is_interface(target) {
            let any = matches!(self.tt.get(target), Type::Any);
            if any || (self.struct_of(src).is_some() && self.missing_methods(src, target).is_empty()) || self.param_implements(src, target) || self.iface_subset(src, target) {
                if self.tt.is_fallible(src) || matches!(self.tt.get(src), Type::Void) {
                    self.mismatch(src, target, &ctx, e.span);
                    return e;
                }
                let span = e.span;
                return mk(ExprKind::ToInterface { value: Box::new(e) }, target, span);
            }
            if self.struct_of(src).is_some() {
                let problems = self.missing_methods(src, target);
                let (s, t) = (self.tn(src), self.tn(target));
                self.err(format!("{s} does not implement {t}: {}", problems.join(", ")), e.span);
                return mk(e.kind, self.tt.invalid, e.span);
            }
        }
        self.mismatch(src, target, &ctx, e.span);
        mk(e.kind, self.tt.invalid, e.span)
    }

    fn mismatch(&mut self, src: TypeId, target: TypeId, ctx: &Ctx, span: Span) {
        let (s, t) = (self.tn(src), self.tn(target));
        let msg = match ctx {
            Ctx::Assign => format!("cannot assign {s} to {t}"),
            Ctx::Init => format!("cannot assign {s} to variable of type {t}"),
            Ctx::Arg(i, f) => format!("cannot use {s} as {t} in argument {} of {f}", i + 1),
            Ctx::Return => format!("cannot return {s} from a function returning {t}"),
            Ctx::Field(name) => format!("cannot use {s} as {t} for field {name}"),
            Ctx::Elem => format!("cannot use {s} as element of type {t}"),
            Ctx::Other => format!("type mismatch: expected {t}, found {s}"),
        };
        self.err(msg, span);
    }

    /// Rejects untyped `nil` where no type is known (`var x = nil`).
    pub(crate) fn default_type(&mut self, e: hir::Expr) -> hir::Expr {
        if matches!(self.tt.get(e.ty), Type::Nil) {
            self.err("use of untyped nil: give the variable an explicit type", e.span);
            return mk(e.kind, self.tt.invalid, e.span);
        }
        e
    }
}
