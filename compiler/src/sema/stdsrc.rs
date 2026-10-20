//! Parts of the standard library written in Slang: `std.task` (`Channel<T>`,
//! `Mutex`, `WaitGroup`, `Spawn`, `Timeout`, `Context`), `std.net` (TCP
//! over non-blocking sockets) and `std.net.http` (HTTP/1.1 client and
//! server). The sources live in `sema/stdlib_src/*.sl`; like
//! `std.collections` ([`super::collections`]) they are parsed once per
//! process and registered as stdlib templates, so only what a program uses
//! is instantiated, checked and compiled.
//!
//! Rules for the sources (they are instantiated in the scope of the user
//! file that first uses them):
//!
//! * structs, functions and enums become templates, also without type
//!   parameters ("plain" templates, instantiated with no arguments on first
//!   use: `task.Mutex`, `net.Dial`); interfaces are registered as ordinary
//!   interfaces. Top-level `var`/`const` are not supported;
//! * names of the module's own namespace are written short (`Conn`); every
//!   other namespace is written with its full path (`std.strings.Index`,
//!   `std.net.Conn`), because the importing file's imports are in effect;
//!   interface method signatures always use full paths;
//! * the runtime is reached through the stdlib externs of
//!   [`super::stdlib::FUNCS`] whose names start with `rt` (internal).

use super::generics::{Inst, TmplDecl};
use super::*;
use std::sync::OnceLock;

/// The Slang-source modules: (namespace, file name, source).
const MODULES: &[(&str, &str, &str)] = &[
    ("std.task", "task.sl", include_str!("stdlib_src/task.sl")),
    ("std.net", "net.sl", include_str!("stdlib_src/net.sl")),
    ("std.net.http", "http.sl", include_str!("stdlib_src/http.sl")),
];

/// The parsed [`MODULES`] (parsed once per process).
fn module_asts() -> &'static [ast::File] {
    static ASTS: OnceLock<Vec<ast::File>> = OnceLock::new();
    ASTS.get_or_init(|| {
        MODULES
            .iter()
            .map(|(ns, name, src)| {
                let mut diags = crate::diagnostics::Diagnostics::new();
                let tokens = crate::lexer::lex(src, &mut diags);
                let file = crate::parser::parse(tokens, &mut diags);
                match file {
                    Some(mut f) if !diags.has_errors() && f.namespace.to_string() == *ns => {
                        // Interfaces are resolved like user declarations (in
                        // the first user file's scope): without spans, so the
                        // IDE index records nothing for them.
                        for d in &mut f.decls {
                            if let ast::Decl::Interface(it) = d {
                                erase_spans(it);
                            }
                        }
                        f
                    }
                    _ => {
                        let errs: Vec<String> = diags
                            .items
                            .iter()
                            .map(|d| {
                                let line = src[..(d.span.start as usize).min(src.len())].matches('\n').count() + 1;
                                format!("{name}:{line}: {}", d.message)
                            })
                            .collect();
                        panic!("internal error: stdlib source {name} does not parse: {errs:?}")
                    }
                }
            })
            .collect()
    })
}

/// Replaces every span of an interface declaration with [`Span::DUMMY`].
fn erase_spans(it: &mut ast::InterfaceDecl) {
    fn ty(t: &mut ast::TypeExpr) {
        t.span = Span::DUMMY;
        match &mut t.kind {
            ast::TypeKind::Named { path, generics } => {
                path.span = Span::DUMMY;
                for s in &mut path.segments {
                    s.span = Span::DUMMY;
                }
                generics.iter_mut().for_each(ty);
            }
            ast::TypeKind::Fallible(x) | ast::TypeKind::Pointer(x) | ast::TypeKind::Weak(x) | ast::TypeKind::Slice(x) => ty(x),
            ast::TypeKind::Map(k, v) => {
                ty(k);
                ty(v);
            }
            ast::TypeKind::Func { params, ret } => {
                params.iter_mut().for_each(ty);
                if let Some(r) = ret {
                    ty(r);
                }
            }
            ast::TypeKind::Tuple(ts) => ts.iter_mut().for_each(ty),
            _ => {}
        }
    }
    it.name.span = Span::DUMMY;
    it.span = Span::DUMMY;
    for m in &mut it.methods {
        m.name.span = Span::DUMMY;
        m.span = Span::DUMMY;
        for p in &mut m.params {
            p.name.span = Span::DUMMY;
            p.span = Span::DUMMY;
            ty(&mut p.ty);
        }
        if let Some(r) = &mut m.ret {
            ty(r);
        }
    }
    it.embeds.iter_mut().for_each(ty);
}

impl<'a> Sema<'a> {
    /// Registers the declarations of the Slang-source stdlib modules.
    pub(crate) fn register_std_sources(&mut self) {
        if self.files.is_empty() {
            return;
        }
        let saved = (self.cur_file, self.cur_ns);
        for file in module_asts() {
            let ns = self.ensure_ns(&file.namespace.to_string(), Span::DUMMY);
            self.cur_ns = ns;
            self.cur_file = 0;
            for d in &file.decls {
                let (name, t) = match d {
                    ast::Decl::Struct(s) => (&s.name.name, self.new_template(&s.name.name, TmplDecl::Struct(s), &s.generics, Vec::new(), false)),
                    ast::Decl::Func(f) => (&f.name.name, self.new_template(&f.name.name, TmplDecl::Func(f), &f.generics, Vec::new(), false)),
                    ast::Decl::Enum(e) => (&e.name.name, self.new_template(&e.name.name, TmplDecl::Enum(e), &e.generics, Vec::new(), false)),
                    ast::Decl::Interface(it) => {
                        let id = self.interfaces.len() as u32;
                        self.interfaces.push(hir::Interface {
                            name: it.name.name.clone(),
                            full_name: format!("{}.{}", self.nss[ns].path, it.name.name),
                            namespace: hir::NamespaceId(0),
                            methods: Vec::new(),
                            attrs: Vec::new(),
                            exported: true,
                            file: 0,
                            span: Span::DUMMY,
                        });
                        self.iface_src.push(it);
                        self.nss[ns].members.insert(it.name.name.clone(), (Member::Interface(id), Span::DUMMY, 0));
                        continue;
                    }
                    _ => panic!("internal error: unsupported declaration in stdlib source {}", file.namespace.to_string()),
                };
                self.nss[ns].members.insert(name.clone(), (Member::Generic(t), Span::DUMMY, 0));
            }
        }
        (self.cur_file, self.cur_ns) = saved;
    }

    /// True if template `t` is a stdlib template without type parameters.
    fn is_plain_std(&self, t: u32) -> bool {
        let tm = &self.gen.templates[t as usize];
        tm.is_std && tm.params.is_empty()
    }

    /// Instantiates plain stdlib template `t` (no type parameters), or
    /// returns `None` if `t` has type parameters or is no stdlib template.
    pub(crate) fn plain_std_inst(&mut self, t: u32, span: Span) -> Option<Inst> {
        if !self.is_plain_std(t) {
            return None;
        }
        if let Some(i) = self.gen.cache.get(&(t, Vec::new())) {
            return Some(*i);
        }
        let funcs = self.funcs.len();
        let inst = self.instantiate(t, Vec::new(), span)?;
        // `Mutex<>` -> `Mutex` in names (symbols stay unique as they are).
        let fix = |s: &mut String| *s = s.replace("<>", "");
        match inst {
            Inst::Struct(s) => {
                let st = &mut self.structs[s as usize];
                fix(&mut st.name);
                fix(&mut st.full_name);
            }
            Inst::Enum(e) => {
                let en = &mut self.enums[e as usize];
                fix(&mut en.name);
                fix(&mut en.full_name);
            }
            Inst::Func(_) => {}
        }
        for f in &mut self.funcs[funcs..] {
            fix(&mut f.name);
            fix(&mut f.full_name);
        }
        Some(inst)
    }

    /// The type named by plain stdlib template `t` (`task.Mutex`).
    pub(crate) fn plain_std_type(&mut self, t: u32, span: Span) -> Option<TypeId> {
        if !self.is_plain_std(t) || !matches!(self.gen.templates[t as usize].decl, TmplDecl::Struct(_) | TmplDecl::Enum(_)) {
            return None;
        }
        Some(match self.plain_std_inst(t, span) {
            Some(Inst::Struct(s)) => self.struct_ty(s),
            Some(Inst::Enum(e)) => self.enum_ty(e),
            _ => self.tt.invalid,
        })
    }

    /// The struct of plain stdlib struct template `t` (literals `task.Mutex{}`).
    pub(crate) fn plain_std_struct(&mut self, t: u32, span: Span) -> Option<u32> {
        if !matches!(self.gen.templates[t as usize].decl, TmplDecl::Struct(_)) {
            return None;
        }
        match self.plain_std_inst(t, span)? {
            Inst::Struct(s) => Some(s),
            _ => None,
        }
    }

    /// The function of plain stdlib function template `t`, as a value.
    pub(crate) fn plain_std_func_value(&mut self, t: u32, span: Span) -> Option<hir::Expr> {
        if !matches!(self.gen.templates[t as usize].decl, TmplDecl::Func(_)) {
            return None;
        }
        match self.plain_std_inst(t, span)? {
            Inst::Func(f) => {
                let ty = self.func_value_ty(f);
                Some(mk(hir::ExprKind::FuncRef(f), ty, span))
            }
            _ => None,
        }
    }

    /// `task.Channel<int>(8)`: calling a stdlib generic struct type calls
    /// its static `New` method. `None` if `t` is no stdlib struct template.
    pub(crate) fn std_type_call(&mut self, t: u32, targs: Option<&[ast::TypeExpr]>, args: &[ast::Expr], span: Span) -> Option<hir::Expr> {
        let tm = &self.gen.templates[t as usize];
        if !tm.is_std || !matches!(tm.decl, TmplDecl::Struct(_)) {
            return None;
        }
        let name = tm.name.clone();
        let sid = match targs {
            Some(ta) => {
                let Some(ts) = self.type_args(t, ta, span) else { return Some(self.invalid(span)) };
                match self.instantiate(t, ts, span) {
                    Some(Inst::Struct(s)) => s,
                    _ => return Some(self.invalid(span)),
                }
            }
            None => match self.plain_std_inst(t, span) {
                Some(Inst::Struct(s)) => s,
                _ => {
                    self.err(format!("missing type arguments for generic type {name} (write {name}<...>(...))"), span);
                    self.check_args_loose(args);
                    return Some(self.invalid(span));
                }
            },
        };
        let ty = self.struct_ty(sid);
        let new = ast::Ident { name: "New".to_string(), span };
        Some(self.type_member_call(ty, &new, args, span))
    }
}
