//! Semantic analysis: name resolution, type checking and lowering to HIR.
//!
//! Entry point: [`analyze`]. The phases are
//!
//! 1. [`resolve`]: build the namespace tree (user files merged by namespace
//!    path, plus the stdlib from [`stdlib`]), register declarations,
//!    resolve imports, then resolve declaration signatures (struct fields,
//!    enum payloads, interface methods, function signatures).
//! 2. [`attrs`]: evaluate and check attributes.
//! 3. Globals and constants are typed lazily (on first use or in order).
//! 4. Function bodies are checked and lowered to HIR in one pass
//!    ([`expr`], [`call`], [`stmt`]); lambdas are lifted to functions.
//!
//! Generic declarations ([`generics`]) are checked once against their
//! constraints and monomorphised: every distinct instantiation becomes an
//! ordinary HIR function, struct or enum. `std.collections` is a set of
//! generic structs written in Slang ([`collections`]).
//!
//! Untyped numeric constants are folded by [`consts`] and materialised with
//! the type the context expects, as in Go.

mod attrs;
mod call;
mod coerce;
mod collections;
mod consts;
mod expr;
mod generics;
pub mod index;
mod infer;
mod ownership;
mod resolve;
mod stmt;
pub mod stdlib;

use crate::ast;
use crate::diagnostics::{Diagnostic, Diagnostics};
use crate::hir::{self, FuncId, GlobalId, Label, LocalId, LocalKind};
use crate::span::Span;
use crate::types::{Type, TypeId, TypeTable};
use std::collections::HashMap;

pub use consts::Folded;
pub use resolve::{ImportPolicy, PackageScope};

/// Analyses a whole program made of `files` (each with its namespace).
/// Diagnostics carry the index of their file in `files`
/// ([`Diagnostic::file`]). Returns `None` if any error was reported.
pub fn analyze(files: &[ast::File], diags: &mut Diagnostics) -> Option<hir::Program> {
    analyze_with_policy(files, diags, None)
}

/// [`analyze`] with an optional [`ImportPolicy`] restricting which root
/// namespaces each file may reach (strict package isolation, used by
/// package builds; see `crate::package`).
pub fn analyze_with_policy(files: &[ast::File], diags: &mut Diagnostics, policy: Option<&ImportPolicy>) -> Option<hir::Program> {
    let mut s = Sema::new(files, diags);
    s.policy = policy;
    s.run();
    let mut had_errors = s.diags.has_errors();
    let program = s.finish();
    if !had_errors {
        ownership::check(&program, diags);
        had_errors = diags.has_errors();
    }
    if had_errors {
        None
    } else {
        Some(program)
    }
}

/// Index into `Sema::nss` (user and stdlib namespaces).
pub(crate) type NsIdx = usize;

/// A name declared in a namespace.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum Member {
    Struct(u32),
    Interface(u32),
    Enum(u32),
    Func(FuncId),
    Global(GlobalId),
    /// Index into [`stdlib::FUNCS`].
    StdFunc(usize),
    Special(stdlib::Special),
    /// Index into [`stdlib::CONSTS`].
    StdConst(usize),
    /// A child namespace.
    Ns(NsIdx),
    /// A generic function, struct or enum (index into `GenState::templates`).
    Generic(u32),
}

/// A namespace during analysis.
pub(crate) struct Ns {
    pub path: String,
    pub members: HashMap<String, (Member, Span, usize)>,
    pub is_std: bool,
    pub hir_id: Option<hir::NamespaceId>,
    pub attrs: Vec<hir::Attr>,
}

/// Builtin functions in scope without imports.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum BuiltinFn {
    Error,
    Len,
    Append,
    Delete,
    Panic,
    Typeof,
}

/// An entry of the file-level import scope.
#[derive(Clone, Debug)]
pub(crate) enum ImportEntry {
    /// `import a.b`: members of the namespace are visible by short name.
    Glob(NsIdx),
    /// `import a.b as x` or `import a.b.Type [as x]`.
    Named(String, Member, NsIdx),
}

/// Per-file context.
pub(crate) struct FileScope {
    pub ns: NsIdx,
    pub imports: Vec<ImportEntry>,
    /// Per entry of `imports`: the `import` line's path span and whether a
    /// lookup went through it (unused imports are errors).
    pub import_uses: Vec<(Span, bool)>,
}

/// Signature of a user function (for calls), receiver excluded.
#[derive(Clone, Debug)]
pub(crate) struct Sig {
    pub params: Vec<TypeId>,
    pub ret: TypeId,
    pub is_async: bool,
}

/// Where a declaration's source lives, for later body checking.
#[derive(Clone, Copy)]
pub(crate) enum FuncSrc<'a> {
    Free(&'a ast::FuncDecl),
    Method(&'a ast::FuncDecl, u32),
    /// Lambdas and synthesized thunks (checked when encountered).
    Synthetic,
}

/// Lazy typing state of a global.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum GState {
    Pending,
    InProgress,
    Done,
}

/// Source of a global var/const.
#[derive(Clone, Copy)]
pub(crate) enum GlobalSrc<'a> {
    Var(&'a ast::VarDecl),
    Const(&'a ast::ConstDecl),
}

/// Entries of a local scope.
#[derive(Clone, Debug)]
pub(crate) enum ScopeEntry {
    Local(LocalId),
    Const(Folded),
}

/// Kind of a break target.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum LabelKind {
    Loop,
    Switch,
}

/// State of one function body being checked (a stack of these handles
/// nested lambdas).
pub(crate) struct FnCtx {
    pub func: Option<FuncId>,
    pub locals: Vec<hir::Local>,
    pub used: Vec<bool>,
    pub scopes: Vec<Vec<(String, ScopeEntry)>>,
    /// Declared result type (`T`, `T!`, `void`); `None` while inferring an
    /// arrow lambda's result.
    pub ret: TypeId,
    pub is_async: bool,
    /// Name for diagnostics.
    pub name: String,
    pub captures: Vec<hir::Capture>,
    /// (depth of the owning FnCtx, local there) for each capture.
    pub capture_keys: Vec<(usize, LocalId)>,
    pub labels: Vec<(Label, LabelKind)>,
    pub next_label: u32,
    /// Number of enclosing block `try` bodies (a `?` inside one jumps to
    /// its `catch`, so it is allowed in any function).
    pub try_depth: u32,
}

impl FnCtx {
    fn new(func: Option<FuncId>, ret: TypeId, is_async: bool, name: String) -> FnCtx {
        FnCtx {
            func,
            locals: Vec::new(),
            used: Vec::new(),
            scopes: vec![Vec::new()],
            ret,
            is_async,
            name,
            captures: Vec::new(),
            capture_keys: Vec::new(),
            labels: Vec::new(),
            next_label: 0,
            try_depth: 0,
        }
    }

    fn lookup(&self, name: &str) -> Option<&ScopeEntry> {
        self.scopes.iter().rev().find_map(|s| s.iter().rev().find(|(n, _)| n == name).map(|(_, e)| e))
    }
}

/// How a value reached by name is accessed from the current function.
#[derive(Clone, Copy, Debug)]
pub(crate) enum VarRef {
    Local(LocalId),
    Upvalue(u32),
}

/// The analyser state.
pub(crate) struct Sema<'a> {
    pub files: &'a [ast::File],
    pub diags: &'a mut Diagnostics,
    pub tt: TypeTable,
    pub nss: Vec<Ns>,
    pub ns_index: HashMap<String, NsIdx>,
    /// Root namespaces (`std`, `app`, ...).
    pub roots: HashMap<String, NsIdx>,
    pub file_scopes: Vec<FileScope>,
    pub structs: Vec<hir::Struct>,
    pub struct_src: Vec<&'a ast::StructDecl>,
    pub interfaces: Vec<hir::Interface>,
    pub iface_src: Vec<&'a ast::InterfaceDecl>,
    pub enums: Vec<hir::Enum>,
    pub enum_src: Vec<&'a ast::EnumDecl>,
    pub funcs: Vec<hir::Function>,
    pub func_src: Vec<FuncSrc<'a>>,
    pub sigs: Vec<Sig>,
    pub globals: Vec<hir::Global>,
    pub global_src: Vec<GlobalSrc<'a>>,
    pub global_state: Vec<GState>,
    /// Folded values of constant globals.
    pub global_folded: HashMap<u32, Folded>,
    /// Globals in the order their typing completed (dependencies first).
    pub global_order: Vec<GlobalId>,
    /// Globals whose initialization cycle was already reported.
    pub cycle_reported: std::collections::HashSet<u32>,
    pub externs: Vec<hir::Extern>,
    pub extern_map: HashMap<String, hir::ExternId>,
    /// Current file and namespace.
    pub cur_file: usize,
    pub cur_ns: NsIdx,
    /// Function contexts, innermost last.
    pub fns: Vec<FnCtx>,
    pub lambda_count: u32,
    /// Methods of enums by (enum id, name). They are ordinary functions
    /// whose first parameter is `this` (the enum value).
    pub enum_methods: HashMap<(u32, String), FuncId>,
    /// The enum owning each enum method (by function id).
    pub enum_method_owner: HashMap<u32, u32>,
    /// Package isolation hook (see [`ImportPolicy`]); `None` = no limits.
    pub policy: Option<&'a ImportPolicy>,
    /// IDE symbol recording (see [`index`]); `None` = off.
    pub(crate) ide: Option<Box<index::Recorder>>,
    /// Generics state (templates, instantiations, type environment).
    pub(crate) gen: generics::GenState<'a>,
}

impl<'a> Sema<'a> {
    fn new(files: &'a [ast::File], diags: &'a mut Diagnostics) -> Sema<'a> {
        Sema {
            files,
            diags,
            tt: TypeTable::new(),
            nss: Vec::new(),
            ns_index: HashMap::new(),
            roots: HashMap::new(),
            file_scopes: Vec::new(),
            structs: Vec::new(),
            struct_src: Vec::new(),
            interfaces: Vec::new(),
            iface_src: Vec::new(),
            enums: Vec::new(),
            enum_src: Vec::new(),
            funcs: Vec::new(),
            func_src: Vec::new(),
            sigs: Vec::new(),
            globals: Vec::new(),
            global_src: Vec::new(),
            global_state: Vec::new(),
            global_folded: HashMap::new(),
            cycle_reported: Default::default(),
            global_order: Vec::new(),
            externs: Vec::new(),
            extern_map: HashMap::new(),
            cur_file: 0,
            cur_ns: 0,
            fns: Vec::new(),
            lambda_count: 0,
            enum_methods: HashMap::new(),
            enum_method_owner: HashMap::new(),
            policy: None,
            ide: None,
            gen: generics::GenState::default(),
        }
    }

    fn run(&mut self) {
        self.build_stdlib();
        self.collect_decls();
        self.add_std_structs();
        self.register_collections();
        self.resolve_imports();
        self.resolve_template_bounds();
        self.resolve_signatures();
        self.run_deferred_bounds();
        self.check_attributes();
        for g in 0..self.globals.len() {
            self.ensure_global(GlobalId(g as u32));
        }
        self.check_templates();
        // Instantiations append functions while bodies are checked; they
        // are checked in turn.
        let mut f = 0;
        while f < self.func_src.len() {
            self.check_function_body(FuncId(f as u32));
            f += 1;
        }
        self.check_unused_imports();
        if let Some(main) = self.find_entry() {
            let sig = &self.sigs[main.0 as usize];
            let void_ok = matches!(self.tt.get(sig.ret), Type::Void) || matches!(self.tt.fallible_inner(sig.ret).map(|t| self.tt.get(t)), Some(Type::Void));
            if !sig.params.is_empty() || !void_ok {
                let f = &self.funcs[main.0 as usize];
                self.cur_file = f.file;
                let span = f.span;
                self.err("main must take no parameters and return nothing (or void!)", span);
            }
        }
    }

    fn finish(self) -> hir::Program {
        let mut namespaces = Vec::new();
        for ns in &self.nss {
            if ns.hir_id.is_some() {
                namespaces.push(hir::Namespace { path: ns.path.clone(), attrs: ns.attrs.clone() });
            }
        }
        let entry = self.find_entry();
        hir::Program {
            types: self.tt,
            namespaces,
            structs: self.structs,
            interfaces: self.interfaces,
            enums: self.enums,
            funcs: self.funcs,
            globals: self.globals,
            global_init_order: self.global_order,
            externs: self.externs,
            entry,
            sources: Vec::new(),
        }
    }

    /// `main` of the user namespace with the shortest path declaring one.
    fn find_entry(&self) -> Option<FuncId> {
        let mut best: Option<(usize, FuncId)> = None;
        for ns in self.nss.iter().filter(|n| !n.is_std) {
            if let Some((Member::Func(f), _, _)) = ns.members.get("main") {
                let depth = ns.path.split('.').count();
                if best.is_none_or(|(d, _)| depth < d) {
                    best = Some((depth, *f));
                }
            }
        }
        best.map(|(_, f)| f)
    }

    // -- diagnostics --------------------------------------------------------

    /// Reports an error in the current file (with the generic
    /// instantiation context, if any).
    pub fn err(&mut self, msg: impl Into<String>, span: Span) {
        let d = Diagnostic::error(msg, span).in_file(self.cur_file);
        self.push_diag(d);
    }

    /// Reports an error with a note.
    pub fn err_note(&mut self, msg: impl Into<String>, span: Span, note: impl Into<String>, note_span: Option<Span>) {
        let d = Diagnostic::error(msg, span).with_note(note, note_span).in_file(self.cur_file);
        self.push_diag(d);
    }

    fn push_diag(&mut self, mut d: Diagnostic) {
        if self.gen.mute > 0 {
            return;
        }
        if let Some(n) = &self.gen.note {
            d = d.with_note(n.clone(), None);
        }
        self.diags.push(d);
    }

    /// Spelling of a type for messages.
    pub fn tn(&self, t: TypeId) -> String {
        hir::dump::type_name(&self.tt, t, self)
    }

    // -- function contexts ---------------------------------------------------

    /// The innermost function context. Panics outside of a body (callers
    /// only use it while checking expressions, which always have one).
    pub fn fcx(&mut self) -> &mut FnCtx {
        self.fns.last_mut().expect("no function context")
    }

    /// Declares a new local in the innermost function and scope.
    pub fn declare_local(&mut self, name: &str, ty: TypeId, kind: LocalKind, span: Span) -> LocalId {
        let dup = self.fns.last().and_then(|f| f.scopes.last()).is_some_and(|s| s.iter().any(|(n, _)| n == name));
        if dup && name != "_" && kind != LocalKind::Temp {
            self.err(format!("{name} redeclared in this block"), span);
        }
        let f = self.fcx();
        let id = LocalId(f.locals.len() as u32);
        f.locals.push(hir::Local { name: name.to_string(), ty, kind, captured: false, address_taken: false, span });
        // Parameters, pattern bindings, catch variables and temps never count
        // as unused.
        f.used.push(!matches!(kind, LocalKind::Var | LocalKind::LoopVar));
        if kind != LocalKind::Temp && name != "_" {
            f.scopes.last_mut().unwrap().push((name.to_string(), ScopeEntry::Local(id)));
            self.ide_def_local(name, id, span);
        }
        id
    }

    /// Declares a fresh compiler temporary.
    pub fn temp(&mut self, ty: TypeId, span: Span) -> LocalId {
        self.declare_local("$tmp", ty, LocalKind::Temp, span)
    }

    pub fn push_scope(&mut self) {
        self.fcx().scopes.push(Vec::new());
    }

    /// Pops a scope, reporting unused variables.
    pub fn pop_scope(&mut self) {
        let scope = self.fcx().scopes.pop().unwrap_or_default();
        for (name, e) in scope {
            if let ScopeEntry::Local(l) = e {
                let f = self.fns.last().unwrap();
                // A variable with a destructor is used by its destructor.
                if !f.used[l.0 as usize] && !self.type_has_dtor(f.locals[l.0 as usize].ty) {
                    let span = f.locals[l.0 as usize].span;
                    self.err(format!("unused variable {name}: declared and never used"), span);
                }
            }
        }
    }

    /// Looks a name up in local scopes, innermost function first, creating
    /// capture chains for outer functions' locals. Marks the local used
    /// when `mark_used`.
    pub fn lookup_local(&mut self, name: &str, mark_used: bool) -> Option<Result<VarRef, Folded>> {
        let depth = self.fns.len();
        for d in (0..depth).rev() {
            let entry = self.fns[d].lookup(name).cloned();
            match entry {
                None => continue,
                Some(ScopeEntry::Const(c)) => return Some(Err(c)),
                Some(ScopeEntry::Local(l)) => {
                    if mark_used {
                        self.fns[d].used[l.0 as usize] = true;
                    }
                    if d == depth - 1 {
                        return Some(Ok(VarRef::Local(l)));
                    }
                    return Some(Ok(VarRef::Upvalue(self.capture_chain(d, l))));
                }
            }
        }
        None
    }

    /// Makes local `l` of function context `d` available in the innermost
    /// lambda; returns the upvalue index there.
    fn capture_chain(&mut self, d: usize, l: LocalId) -> u32 {
        self.fns[d].locals[l.0 as usize].captured = true;
        let name = self.fns[d].locals[l.0 as usize].name.clone();
        let ty = self.fns[d].locals[l.0 as usize].ty;
        let mut source = hir::CaptureSource::Local(l);
        let mut idx = 0;
        for j in d + 1..self.fns.len() {
            let f = &mut self.fns[j];
            idx = match f.capture_keys.iter().position(|k| *k == (d, l)) {
                Some(i) => i as u32,
                None => {
                    f.capture_keys.push((d, l));
                    f.captures.push(hir::Capture { name: name.clone(), ty, source });
                    (f.captures.len() - 1) as u32
                }
            };
            source = hir::CaptureSource::Upvalue(idx);
        }
        idx
    }

    /// Type of a local/upvalue reference.
    pub fn var_ty(&self, v: VarRef) -> TypeId {
        let f = self.fns.last().unwrap();
        match v {
            VarRef::Local(l) => f.locals[l.0 as usize].ty,
            VarRef::Upvalue(i) => f.captures[i as usize].ty,
        }
    }

    /// Marks a local as having its address taken (follows captures).
    pub fn mark_address_taken(&mut self, e: &hir::Expr) {
        match &e.kind {
            hir::ExprKind::Local(l) => {
                if let Some(f) = self.fns.last_mut() {
                    f.locals[l.0 as usize].address_taken = true;
                }
            }
            hir::ExprKind::Field { base, .. } | hir::ExprKind::Index { base, .. } => {
                if matches!(self.tt.get(base.ty), Type::Struct(_) | Type::Array(..)) {
                    self.mark_address_taken(base)
                }
            }
            _ => {}
        }
    }

    /// Allocates a label for a loop or switch and pushes it.
    pub fn push_label(&mut self, kind: LabelKind) -> Label {
        let f = self.fcx();
        let l = Label(f.next_label);
        f.next_label += 1;
        f.labels.push((l, kind));
        l
    }

    pub fn pop_label(&mut self) {
        self.fcx().labels.pop();
    }

    /// Innermost break target / loop.
    pub fn break_target(&mut self) -> Option<Label> {
        self.fns.last().and_then(|f| f.labels.last().map(|l| l.0))
    }

    pub fn continue_target(&mut self) -> Option<Label> {
        self.fns.last().and_then(|f| f.labels.iter().rev().find(|l| l.1 == LabelKind::Loop).map(|l| l.0))
    }

    /// Adds (or reuses) an extern for a stdlib function.
    pub fn extern_id(&mut self, path: String, symbol: &str, params: Vec<TypeId>, ret: TypeId) -> hir::ExternId {
        if let Some(&id) = self.extern_map.get(symbol) {
            return id;
        }
        let id = hir::ExternId(self.externs.len() as u32);
        self.externs.push(hir::Extern { path, symbol: symbol.to_string(), params, ret });
        self.extern_map.insert(symbol.to_string(), id);
        id
    }

    /// Reports imports no name was looked up through (like Go). Not when
    /// other errors were found (a failed lookup may hide the use). Imports
    /// of a root namespace (`import std`) are exempt: they only make child
    /// namespaces reachable by short name and are customary boilerplate.
    fn check_unused_imports(&mut self) {
        if self.diags.has_errors() {
            return;
        }
        for fi in 0..self.file_scopes.len() {
            for (span, used) in self.file_scopes[fi].import_uses.clone() {
                let imp = self.files[fi].imports.iter().find(|i| i.path.span == span);
                let root = imp.is_some_and(|i| i.path.segments.len() == 1 && i.alias.is_none());
                if !used && !root {
                    self.cur_file = fi;
                    let path = imp.map(|i| i.path.to_string()).unwrap_or_default();
                    self.err(format!("unused import {path}: nothing from it is used"), span);
                }
            }
        }
    }

    /// Checks one function body (no-op for synthetic functions).
    fn check_function_body(&mut self, fid: FuncId) {
        let decl = match self.func_src[fid.0 as usize] {
            FuncSrc::Free(d) => d,
            FuncSrc::Method(d, _) => d,
            FuncSrc::Synthetic => return,
        };
        let Some(body) = &decl.body else { return };
        if self.gen.insts.contains_key(&fid.0) {
            self.check_instance_body(fid, decl, body);
            return;
        }
        let f = &self.funcs[fid.0 as usize];
        self.cur_file = f.file;
        self.cur_ns = self.ns_of_hir(f.namespace);
        self.check_body_in_scope(fid, decl, body);
    }

    /// Checks a function body in the current file/namespace scope.
    pub(crate) fn check_body_in_scope(&mut self, fid: FuncId, decl: &ast::FuncDecl, body: &ast::Block) {
        let f = &self.funcs[fid.0 as usize];
        let sig = self.sigs[fid.0 as usize].clone();
        let mut ctx = FnCtx::new(Some(fid), sig.ret, sig.is_async, f.name.clone());
        // Parameters were registered as locals by `resolve_signatures`.
        ctx.locals = f.locals.clone();
        ctx.used = vec![true; ctx.locals.len()];
        for (i, l) in f.locals.iter().enumerate() {
            if l.name != "_" {
                ctx.scopes[0].push((l.name.clone(), ScopeEntry::Local(LocalId(i as u32))));
            }
        }
        self.fns.push(ctx);
        let block = self.fn_body(body, sig.ret, decl.name.span);
        let ctx = self.fns.pop().unwrap();
        let f = &mut self.funcs[fid.0 as usize];
        f.locals = ctx.locals;
        f.body = block;
    }

    /// Checks a function body block and the missing-return rule.
    pub fn fn_body(&mut self, body: &ast::Block, ret: TypeId, name_span: Span) -> hir::Block {
        let block = self.block(body);
        let needs_return = !matches!(self.tt.get(ret), Type::Void | Type::Invalid)
            && !matches!(self.tt.fallible_inner(ret).map(|t| self.tt.get(t)), Some(Type::Void));
        if needs_return && !stmt::terminates(&block.stmts, false) {
            let name = self.fns.last().map(|f| f.name.clone()).unwrap_or_default();
            let end = Span::new(body.span.end.saturating_sub(1), body.span.end);
            self.err_note(
                format!("missing return at end of function {name} returning {}", self.tn(ret)),
                end,
                "every path must end in return or panic (an if needs an else; a switch needs a default)",
                Some(name_span),
            );
        }
        block
    }

    /// Maps a HIR namespace id back to the analyser's namespace index.
    pub fn ns_of_hir(&self, id: hir::NamespaceId) -> NsIdx {
        self.nss.iter().position(|n| n.hir_id == Some(id)).unwrap_or(0)
    }

    /// Placeholder function (filled in later).
    pub fn dummy_function(&self, name: String, kind: hir::FuncKind, ns: hir::NamespaceId) -> hir::Function {
        hir::Function {
            name: name.clone(),
            full_name: name.clone(),
            symbol: format!("slang_{}", name.replace('.', "__")),
            kind,
            namespace: ns,
            params: Vec::new(),
            ret: self.tt.void,
            is_async: false,
            locals: Vec::new(),
            captures: Vec::new(),
            body: hir::Block::default(),
            attrs: Vec::new(),
            exported: false,
            file: self.cur_file,
            span: Span::DUMMY,
        }
    }

    /// The HIR namespace id of the current namespace.
    pub fn cur_hir_ns(&self) -> hir::NamespaceId {
        self.nss[self.cur_ns].hir_id.unwrap_or(hir::NamespaceId(0))
    }

    /// Runs `f` with the file/namespace context of `file`, saving and
    /// restoring the current function stack.
    pub fn in_file<R>(&mut self, file: usize, f: impl FnOnce(&mut Self) -> R) -> R {
        let saved = (self.cur_file, self.cur_ns, std::mem::take(&mut self.fns), std::mem::take(&mut self.gen.tenv), self.gen.note.take());
        self.cur_file = file;
        self.cur_ns = self.file_scopes[file].ns;
        let r = f(self);
        self.cur_file = saved.0;
        self.cur_ns = saved.1;
        self.fns = saved.2;
        self.gen.tenv = saved.3;
        self.gen.note = saved.4;
        r
    }
}

impl hir::dump::DeclNames for Sema<'_> {
    fn struct_name(&self, id: u32) -> String {
        self.structs.get(id as usize).map(|s| s.full_name.clone()).unwrap_or_default()
    }
    fn interface_name(&self, id: u32) -> String {
        self.interfaces.get(id as usize).map(|s| s.full_name.clone()).unwrap_or_default()
    }
    fn enum_name(&self, id: u32) -> String {
        self.enums.get(id as usize).map(|s| s.full_name.clone()).unwrap_or_default()
    }
    fn param_name(&self, id: u32) -> String {
        self.gen.tparams.get(id as usize).map(|p| p.name.clone()).unwrap_or_default()
    }
}

/// True if `name` is exported (starts with an uppercase letter).
pub(crate) fn is_exported(name: &str) -> bool {
    name.chars().next().is_some_and(|c| c.is_uppercase())
}

/// Shorthand for a HIR expression.
pub(crate) fn mk(kind: hir::ExprKind, ty: TypeId, span: Span) -> hir::Expr {
    hir::Expr { kind, ty, span }
}
