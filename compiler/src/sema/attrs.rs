//! Attribute resolution and checking.
//!
//! An attribute is either a builtin (`@attribute`, `@reflect`,
//! `@deprecated`, `@inline`, `@test`, `@json`) or a struct declared with
//! `@attribute`, whose fields are its parameters. Arguments are positional
//! (in field order) or named, must be compile-time constants, and are stored
//! evaluated in [`hir::Attr`].

use super::resolve::{PathRes, BUILTIN_ATTRS};
use super::*;
use crate::hir::{Attr, AttrKind, ConstValue};

/// Declaration kinds attributes can target (`@attribute(targets: [...])`).
pub(crate) const TARGETS: &[&str] = &["Namespace", "Struct", "Interface", "Enum", "Variant", "Field", "Func", "Param"];

/// Parameter kinds of builtin attributes.
#[derive(Clone, Copy, PartialEq)]
enum PKind {
    Str,
    Bool,
    Targets,
}

fn builtin_params(name: &str) -> &'static [(&'static str, PKind)] {
    match name {
        "attribute" => &[("targets", PKind::Targets), ("repeatable", PKind::Bool)],
        "deprecated" => &[("message", PKind::Str)],
        "json" => &[("name", PKind::Str), ("omitEmpty", PKind::Bool), ("skip", PKind::Bool)],
        _ => &[],
    }
}

impl<'a> Sema<'a> {
    /// Evaluates the attributes of every declaration.
    pub(crate) fn check_attributes(&mut self) {
        // Attribute structs first: their own `@attribute(...)` and field
        // defaults are needed to check uses.
        let order: Vec<usize> = (0..self.structs.len()).filter(|&s| self.structs[s].is_attribute).chain((0..self.structs.len()).filter(|&s| !self.structs[s].is_attribute)).collect();
        for sid in order {
            let src = self.struct_src[sid];
            self.cur_file = self.structs[sid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            let attrs = self.eval_attrs(&src.attrs, "Struct");
            self.structs[sid].attrs = attrs;
            for f in &src.fields {
                let attrs = self.eval_attrs(&f.attrs, "Field");
                let ty = self.structs[sid].fields.iter().find(|x| x.name == f.name.name).map(|x| x.ty);
                let default = match (&f.default, ty) {
                    (Some(d), Some(ty)) => self.attr_value(d, ty),
                    _ => None,
                };
                if let Some(field) = self.structs[sid].fields.iter_mut().find(|x| x.name == f.name.name) {
                    field.attrs = attrs;
                    field.default = default;
                }
            }
        }
        let files = self.files;
        for (fi, file) in files.iter().enumerate() {
            self.cur_file = fi;
            self.cur_ns = self.file_scopes[fi].ns;
            let attrs = self.eval_attrs(&file.attrs, "Namespace");
            let ns = self.cur_ns;
            self.nss[ns].attrs.extend(attrs);
        }
        for iid in 0..self.interfaces.len() {
            self.cur_file = self.interfaces[iid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            let src = self.iface_src[iid];
            let attrs = self.eval_attrs(&src.attrs, "Interface");
            self.interfaces[iid].attrs = attrs;
        }
        for eid in 0..self.enums.len() {
            self.cur_file = self.enums[eid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            let src = self.enum_src[eid];
            let attrs = self.eval_attrs(&src.attrs, "Enum");
            self.enums[eid].attrs = attrs;
            for v in &src.variants {
                let attrs = self.eval_attrs(&v.attrs, "Variant");
                if let Some(hv) = self.enums[eid].variants.iter_mut().find(|x| x.name == v.name.name) {
                    hv.attrs = attrs;
                }
                for f in &v.fields {
                    self.eval_attrs(&f.attrs, "Field");
                }
            }
        }
        for fid in 0..self.funcs.len() {
            let decl = match self.func_src[fid] {
                FuncSrc::Free(d) | FuncSrc::Method(d, _) => d,
                FuncSrc::Synthetic => continue,
            };
            self.cur_file = self.funcs[fid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            let attrs = self.eval_attrs(&decl.attrs, "Func");
            self.funcs[fid].attrs = attrs;
            for p in &decl.params {
                // Parameter attributes are checked; HIR keeps no per-param list yet.
                self.eval_attrs(&p.attrs, "Param");
            }
        }
        for gid in 0..self.globals.len() {
            self.cur_file = self.globals[gid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            let attrs = match self.global_src[gid] {
                GlobalSrc::Var(v) => &v.attrs,
                GlobalSrc::Const(c) => &c.attrs,
            };
            let attrs = self.eval_attrs(attrs, "Var");
            self.globals[gid].attrs = attrs;
        }
    }

    /// Resolves the attribute's declaration: a user `@attribute` struct in
    /// scope, else a builtin.
    fn attr_kind(&mut self, a: &ast::Attribute) -> Option<AttrKind> {
        let segs: Vec<&ast::Ident> = a.path.segments.iter().collect();
        let name = a.path.to_string();
        if let Some((PathRes::Member(Member::Struct(s)), n)) = self.resolve_segments(&segs, false) {
            if n == segs.len() {
                if self.structs[s as usize].is_attribute {
                    self.resolve_segments(&segs, true);
                    return Some(AttrKind::User(s));
                }
                self.err(format!("{name} is not an attribute (declare the struct with @attribute)"), a.path.span);
                return None;
            }
        }
        if segs.len() == 1 && BUILTIN_ATTRS.contains(&name.as_str()) {
            return Some(AttrKind::Builtin(name));
        }
        self.err(format!("unknown attribute @{name}"), a.path.span);
        None
    }

    /// Evaluates a list of attributes on a declaration of kind `target`.
    pub(crate) fn eval_attrs(&mut self, attrs: &[ast::Attribute], target: &str) -> Vec<Attr> {
        let mut out: Vec<Attr> = Vec::new();
        for a in attrs {
            let Some(kind) = self.attr_kind(a) else { continue };
            let args = match &kind {
                AttrKind::Builtin(n) => self.builtin_args(n, a),
                AttrKind::User(s) => self.user_args(*s, a),
            };
            let (targets, repeatable) = self.attr_meta(&kind);
            let name = a.path.to_string();
            if let AttrKind::Builtin(n) = &kind {
                if n == "attribute" && target != "Struct" {
                    self.err("@attribute can only be applied to a struct", a.span);
                }
            }
            if let Some(ts) = targets {
                if !ts.iter().any(|t| t == target) {
                    self.err(format!("attribute @{name} cannot be applied to a {} (allowed: {})", target.to_lowercase(), ts.join(", ")), a.span);
                }
            }
            if !repeatable && out.iter().any(|o| o.kind == kind) {
                self.err(format!("attribute @{name} is not repeatable"), a.span);
            }
            out.push(Attr { kind, args, span: a.span });
        }
        out
    }

    /// (allowed targets, repeatable) of an attribute kind.
    fn attr_meta(&self, kind: &AttrKind) -> (Option<Vec<String>>, bool) {
        let AttrKind::User(s) = kind else { return (None, false) };
        let Some(meta) = self.structs[*s as usize].attrs.iter().find(|a| a.kind == AttrKind::Builtin("attribute".into())) else {
            return (None, false);
        };
        let mut targets = None;
        let mut repeatable = false;
        for (k, v) in &meta.args {
            match (k.as_str(), v) {
                ("targets", ConstValue::List(vs)) => {
                    targets = Some(vs.iter().filter_map(|v| if let ConstValue::Name(n) = v { Some(n.clone()) } else { None }).collect())
                }
                ("repeatable", ConstValue::Bool(b)) => repeatable = *b,
                _ => {}
            }
        }
        (targets, repeatable)
    }

    /// Matches arguments to parameter names: positional by order, or named.
    fn bind_args<'e>(&mut self, a: &'e ast::Attribute, params: &[String]) -> Vec<(usize, &'e ast::Expr)> {
        let mut out: Vec<(usize, &ast::Expr)> = Vec::new();
        let name = a.path.to_string();
        for (i, arg) in a.args.iter().enumerate() {
            let idx = match &arg.name {
                Some(n) => match params.iter().position(|p| *p == n.name) {
                    Some(i) => i,
                    None => {
                        self.err(format!("unknown argument {} for attribute @{name}", n.name), n.span);
                        continue;
                    }
                },
                None => {
                    if i >= params.len() {
                        self.err(format!("too many arguments for attribute @{name}"), arg.span);
                        continue;
                    }
                    i
                }
            };
            if out.iter().any(|(j, _)| *j == idx) {
                self.err(format!("argument {} given twice for attribute @{name}", params[idx]), arg.span);
                continue;
            }
            out.push((idx, &arg.value));
        }
        out
    }

    fn builtin_args(&mut self, n: &str, a: &ast::Attribute) -> Vec<(String, ConstValue)> {
        let params = builtin_params(n);
        let names: Vec<String> = params.iter().map(|p| p.0.to_string()).collect();
        let mut out = Vec::new();
        for (idx, e) in self.bind_args(a, &names) {
            let (pname, kind) = params[idx];
            let v = match kind {
                PKind::Targets => self.targets_value(e),
                PKind::Str => self.attr_value(e, self.tt.string),
                PKind::Bool => self.attr_value(e, self.tt.bool_),
            };
            if let Some(v) = v {
                out.push((pname.to_string(), v));
            }
        }
        out
    }

    fn user_args(&mut self, s: u32, a: &ast::Attribute) -> Vec<(String, ConstValue)> {
        let fields: Vec<(String, TypeId, Option<ConstValue>)> = self.structs[s as usize].fields.iter().map(|f| (f.name.clone(), f.ty, f.default.clone())).collect();
        let names: Vec<String> = fields.iter().map(|f| f.0.clone()).collect();
        let bound = self.bind_args(a, &names);
        let mut out = Vec::new();
        for (i, (fname, ty, default)) in fields.iter().enumerate() {
            match bound.iter().find(|(j, _)| *j == i) {
                Some((_, e)) => {
                    if let Some(v) = self.attr_value(e, *ty) {
                        out.push((fname.clone(), v));
                    }
                }
                None => match default {
                    Some(d) => out.push((fname.clone(), d.clone())),
                    None => self.err(format!("missing argument {fname} for attribute @{}", a.path.to_string()), a.span),
                },
            }
        }
        out
    }

    /// `[reflect.Target.Field, ...]`: the last path segment of each element.
    fn targets_value(&mut self, e: &ast::Expr) -> Option<ConstValue> {
        let ast::ExprKind::SliceLit(es) = &e.kind else {
            self.err("targets must be a list like [reflect.Target.Field]", e.span);
            return None;
        };
        let mut out = Vec::new();
        for x in es {
            match Self::flatten_path(x).and_then(|s| s.last().map(|i| i.name.clone())) {
                Some(n) if TARGETS.contains(&n.as_str()) => out.push(ConstValue::Name(n)),
                _ => self.err(format!("unknown attribute target (expected one of {})", TARGETS.join(", ")), x.span),
            }
        }
        Some(ConstValue::List(out))
    }

    /// A compile-time constant of type `ty` (primitives, strings, plain
    /// enums, slices of those).
    pub(crate) fn attr_value(&mut self, e: &ast::Expr, ty: TypeId) -> Option<ConstValue> {
        if let (ast::ExprKind::SliceLit(es), Type::Slice(el)) = (&e.kind, self.tt.get(ty).clone()) {
            let vs: Option<Vec<ConstValue>> = es.iter().map(|x| self.attr_value(x, el)).collect();
            return vs.map(ConstValue::List);
        }
        let Some(f) = self.fold(e) else {
            self.err("attribute arguments must be compile-time constants", e.span);
            return None;
        };
        let h = self.const_expr(f, Some(ty), e.span);
        if h.ty != ty {
            self.err(format!("attribute argument has type {}, want {}", self.tn(h.ty), self.tn(ty)), e.span);
            return None;
        }
        match h.kind {
            hir::ExprKind::Const(v) => Some(v),
            hir::ExprKind::EnumLit { enum_id, variant, .. } => Some(ConstValue::Enum(enum_id, variant)),
            _ => None,
        }
    }
}
