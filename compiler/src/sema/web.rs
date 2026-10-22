//! Compile-time expansion for `std.web` (docs/FRAMEWORK.md section 2):
//! the route table of controllers and minimal-API handlers and the
//! factories of the DI container are generated here, when every type is
//! known, instead of being discovered by reflection at start-up.
//!
//! * `app.MapController<C>()` (also `MapControllers<A, B, ...>()`) reads
//!   the `@route` of struct `C` and the `@get/@post/@put/@delete/@patch`
//!   attributes of its methods, classifies every method parameter (route
//!   `{name}`, `@query`, `@header`, `@form`, `@body`, `@fromServices`,
//!   `*web.Context`, services by pointer/interface type, structs as the
//!   JSON body, scalars from the query) and generates one endpoint per
//!   action: binding with conversion and `@validate`, construction of the
//!   controller (static `New(deps...)` or injected fields), the call, the
//!   result (`web.Result<T>`, a value, `void`, `T!`), and the OpenAPI
//!   metadata (`json.Schema<T>()` of parameters, body and response).
//! * `app.MapGet(path, handler)` (and `MapPost/MapPut/MapDelete/MapPatch`)
//!   does the same for a lambda or function: parameters are inferred from
//!   its signature (route parameters by name, services by type, the
//!   context if asked for).
//! * `app.Services.AddSingleton<T>()` / `AddScoped<T>()` /
//!   `AddTransient<T>()` (and `<I, T>` for an interface) generate the
//!   factory of `*T`: `T.New(deps...)` with resolved arguments, or the
//!   zero `T` with its `@inject` fields resolved.
//!
//! The generated code is Slang source text of a stdlib template in
//! namespace `std.web` whose type parameters are bound to the concrete
//! types (so no type is ever spelled), parsed once per distinct text with
//! every token placed at the call site, and instantiated like any other
//! stdlib generic. Invalid routes (bad templates, duplicates, a `{param}`
//! without a parameter, unbindable parameter types) are compile errors at
//! the call; so is a controller or service dependency that the program
//! never registers ([`Sema::web_finish`]).

use super::coerce::Ctx;
use super::generics::{Inst, TmplDecl};
use super::*;
use crate::hir::{AttrKind, Callee, ConstValue, ExprKind};

/// State of the expansion.
#[derive(Default)]
pub(crate) struct WebState {
    /// Generated templates by source text.
    tmpls: HashMap<String, u32>,
    /// Routes mapped so far: (normalized "GET /a/{}", written pattern, span, file).
    routes: Vec<(String, String, Span, usize)>,
    /// Service types registered anywhere in the program.
    registered: Vec<TypeId>,
    /// Dependencies to check: (type, owner, span, file).
    required: Vec<(TypeId, String, Span, usize)>,
}

/// Where a handler parameter comes from.
#[derive(Clone, Debug, PartialEq)]
enum Src {
    Path,
    Query(String),
    Header(String),
    Form(String),
    Body,
    Service,
    Context,
}

/// A classified handler parameter.
struct PInfo {
    name: String,
    ty: TypeId,
    src: Src,
    /// `validate{...}` literal of its `@validate`.
    validate: Option<String>,
    required: bool,
}

/// The result shape of a handler.
enum Ret {
    Void,
    /// `web.Result<X>` (X).
    Result(TypeId),
    Value(TypeId),
}

/// Type parameters of a generated template: the concrete types in order.
#[derive(Default)]
struct Tys(Vec<TypeId>);

impl Tys {
    /// The type parameter standing for `t` ("T3").
    fn p(&mut self, t: TypeId) -> String {
        let i = match self.0.iter().position(|x| *x == t) {
            Some(i) => i,
            None => {
                self.0.push(t);
                self.0.len() - 1
            }
        };
        format!("T{i}")
    }

    fn generics(&self) -> String {
        (0..self.0.len()).map(|i| format!("T{i}")).collect::<Vec<_>>().join(", ")
    }
}

/// A Slang string literal.
fn lit(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\t' => out.push_str("\\t"),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// A Slang constant for an attribute value.
fn const_src(v: &ConstValue) -> String {
    match v {
        ConstValue::Int(i) => i.to_string(),
        ConstValue::Float(f) => {
            if f.is_finite() && f.fract() == 0.0 && f.abs() < 1e15 {
                format!("{f:.1}")
            } else {
                format!("{f:e}")
            }
        }
        ConstValue::Bool(b) => b.to_string(),
        ConstValue::Char(c) => format!("'{c}'"),
        ConstValue::Str(s) => lit(s),
        ConstValue::List(vs) => format!("[{}]", vs.iter().map(const_src).collect::<Vec<_>>().join(", ")),
        ConstValue::Enum(..) | ConstValue::Name(_) => "0".into(),
    }
}

/// Segments of a route template; an error message if it is malformed.
fn route_segments(path: &str) -> Result<Vec<String>, String> {
    let segs: Vec<String> = path.split('/').filter(|s| !s.is_empty()).map(|s| s.to_string()).collect();
    for (i, s) in segs.iter().enumerate() {
        if s == "*" {
            if i + 1 != segs.len() {
                return Err("`*` must be the last segment".into());
            }
            continue;
        }
        if s.starts_with('{') || s.ends_with('}') {
            let inner = s.strip_prefix('{').and_then(|x| x.strip_suffix('}'));
            match inner {
                Some(n) if !n.is_empty() && n.chars().all(|c| c.is_alphanumeric() || c == '_') && !n.starts_with(|c: char| c.is_ascii_digit()) => {}
                _ => return Err(format!("malformed parameter segment `{s}` (write `{{name}}`)")),
            }
        } else if s.contains('{') || s.contains('}') {
            return Err(format!("a parameter must be a whole segment: `{s}`"));
        }
    }
    Ok(segs)
}

fn param_names(segs: &[String]) -> Vec<String> {
    segs.iter().filter_map(|s| s.strip_prefix('{').and_then(|x| x.strip_suffix('}')).map(|x| x.to_string())).collect()
}

fn join_route(prefix: &str, rel: &str) -> String {
    if rel.starts_with('/') {
        return rel.to_string();
    }
    let p = prefix.trim_end_matches('/');
    if rel.is_empty() {
        if p.is_empty() {
            "/".into()
        } else {
            p.to_string()
        }
    } else {
        format!("{p}/{rel}")
    }
}

/// The parsed generated source `src` with every token at `span`. Parsed
/// files live for the rest of the process (templates borrow them); they
/// are shared by every analysis that generates the same text at the same
/// place (the language server re-analyses the same files over and over).
fn parsed_template(src: &str, span: Span) -> &'static ast::File {
    use std::sync::{Mutex, OnceLock};
    static CACHE: OnceLock<Mutex<HashMap<(String, u32, u32), &'static ast::File>>> = OnceLock::new();
    let key = (src.to_string(), span.start, span.end);
    let cache = CACHE.get_or_init(|| Mutex::new(HashMap::new()));
    if let Some(f) = cache.lock().unwrap().get(&key) {
        return f;
    }
    let mut d = crate::diagnostics::Diagnostics::new();
    let mut toks = crate::lexer::lex(src, &mut d);
    for t in &mut toks {
        t.span = span;
    }
    let file = match crate::parser::parse(toks, &mut d) {
        Some(f) if !d.has_errors() => f,
        _ => {
            let errs: Vec<String> = d.items.iter().map(|x| x.message.clone()).collect();
            panic!("internal error: generated std.web code does not parse: {errs:?}\n{src}")
        }
    };
    let file: &'static ast::File = Box::leak(Box::new(file));
    cache.lock().unwrap().insert(key, file);
    file
}

const VERBS: &[(&str, &str)] = &[("get", "GET"), ("post", "POST"), ("put", "PUT"), ("delete", "DELETE"), ("patch", "PATCH")];

impl<'a> Sema<'a> {
    // -- entry ------------------------------------------------------------------

    /// Method calls expanded at compile time (see the module documentation);
    /// gives `base` back for every other call.
    pub(crate) fn web_method_call(&mut self, base: hir::Expr, name: &ast::Ident, targs: Option<&[ast::TypeExpr]>, args: &[ast::Expr], span: Span) -> Result<hir::Expr, hir::Expr> {
        let Some(sid) = self.struct_of(base.ty) else { return Err(base) };
        let full = self.structs[sid as usize].full_name.clone();
        let n = name.name.as_str();
        match (full.as_str(), n) {
            ("std.web.App", "MapController" | "MapControllers") => {
                let recv = self.web_recv(base);
                Ok(self.web_map_controllers(recv, targs, args, span))
            }
            ("std.web.App", "MapGet" | "MapPost" | "MapPut" | "MapDelete" | "MapPatch") => {
                let recv = self.web_recv(base);
                let verb = n[3..].to_uppercase();
                Ok(self.web_map_minimal(recv, &verb, args, span))
            }
            ("std.web.Services", "AddSingleton" | "AddScoped" | "AddTransient") => {
                let recv = self.web_recv(base);
                Ok(self.web_add_service(recv, &n[3..], targs, args, span))
            }
            ("std.web.Services", "AddInstance" | "AddSingletonFactory" | "AddScopedFactory" | "AddTransientFactory") => {
                // An ordinary generic method; its type argument is recorded
                // as registered.
                let Some(&t) = self.gen.methods.get(&(sid, name.name.clone())) else { return Err(base) };
                let recv = self.web_recv(base);
                let e = self.generic_call(t, targs, Some(recv), args, span);
                if let ExprKind::Call { callee: Callee::Func(f), .. } = &e.kind {
                    if let Some(ty) = self.gen.insts.get(&f.0).and_then(|i| i.env.iter().find(|(n, _)| n == "T").map(|x| x.1)) {
                        self.web_registered(ty);
                    }
                }
                Ok(e)
            }
            _ => Err(base),
        }
    }

    fn web_recv(&mut self, base: hir::Expr) -> hir::Expr {
        if matches!(self.tt.get(base.ty), Type::Pointer(_)) {
            base
        } else {
            self.addr_of(base)
        }
    }

    // -- generated templates ---------------------------------------------------------

    /// The template of generated source `src` (one function in namespace
    /// std.web), every token at `span`.
    fn web_template(&mut self, src: String, span: Span) -> u32 {
        if let Some(&t) = self.web.tmpls.get(&src) {
            return t;
        }
        if std::env::var_os("SLANG_WEB_DUMP").is_some() {
            eprintln!("{src}");
        }
        let file = parsed_template(&src, span);
        let ast::Decl::Func(f) = &file.decls[0] else { panic!("internal error: generated std.web code is not a function") };
        let saved = self.cur_ns;
        self.cur_ns = self.ensure_ns("std.web", Span::DUMMY);
        let t = self.new_template(&f.name.name, TmplDecl::Func(f), &f.generics, Vec::new(), false);
        self.gen.templates[t as usize].generated = true;
        self.cur_ns = saved;
        self.web.tmpls.insert(src, t);
        t
    }

    /// A call of the generated function `src` instantiated with `tys`.
    fn web_call(&mut self, src: String, tys: Vec<TypeId>, args: Vec<hir::Expr>, span: Span) -> hir::Expr {
        let t = self.web_template(src, span);
        let Some(Inst::Func(fid)) = self.instantiate(t, tys, span) else { return self.invalid(span) };
        let sig = self.sigs[fid.0 as usize].clone();
        let fname = self.funcs[fid.0 as usize].name.clone();
        let args: Vec<hir::Expr> = args.into_iter().zip(sig.params.iter()).enumerate().map(|(i, (a, p))| self.coerce(a, *p, Ctx::Arg(i, fname.clone()))).collect();
        let ty = if sig.is_async { self.tt.task(sig.ret) } else { sig.ret };
        mk(ExprKind::Call { callee: Callee::Func(fid), args }, ty, span)
    }

    fn web_errors(&self) -> usize {
        self.diags.items.iter().filter(|d| d.severity == crate::diagnostics::Severity::Error).count()
    }

    fn web_counter(&self) -> usize {
        self.web.tmpls.len()
    }

    // -- type helpers --------------------------------------------------------------------

    /// Full name of the std.web attribute struct of `a`, if it is one.
    fn web_attr_name(&self, a: &hir::Attr) -> Option<String> {
        match a.kind {
            AttrKind::User(s) => self.structs[s as usize].full_name.strip_prefix("std.web.").map(|x| x.to_string()),
            _ => None,
        }
    }

    fn attr_str(a: &hir::Attr, key: &str) -> String {
        a.args.iter().find(|(k, _)| k == key).and_then(|(_, v)| if let ConstValue::Str(s) = v { Some(s.clone()) } else { None }).unwrap_or_default()
    }

    fn attr_strs(a: &hir::Attr, key: &str) -> Vec<String> {
        match a.args.iter().find(|(k, _)| k == key) {
            Some((_, ConstValue::List(vs))) => vs.iter().filter_map(|v| if let ConstValue::Str(s) = v { Some(s.clone()) } else { None }).collect(),
            _ => Vec::new(),
        }
    }

    /// `validate{...}` source of a `@validate`.
    fn validate_src(a: &hir::Attr) -> String {
        let fields: Vec<String> = a.args.iter().map(|(k, v)| format!("{k}: {}", const_src(v))).collect();
        format!("validate{{{}}}", fields.join(", "))
    }

    fn attr_required(a: &hir::Attr) -> bool {
        a.args.iter().any(|(k, v)| k == "required" && *v == ConstValue::Bool(true))
    }

    /// Scalars bindable from text (route, query, header, form).
    fn web_scalar(&self, t: TypeId) -> bool {
        match self.tt.get(t) {
            Type::Bool | Type::Int(_) | Type::Float32 | Type::Float64 | Type::String => true,
            Type::Enum(d) => self.enums[d.0 as usize].variants.iter().all(|v| v.fields.is_empty()),
            Type::Pointer(x) => {
                let x = *x;
                !matches!(self.tt.get(x), Type::Struct(_) | Type::Pointer(_)) && self.web_scalar(x)
            }
            _ => false,
        }
    }

    fn web_text_bindable(&self, t: TypeId) -> bool {
        match self.tt.get(t) {
            Type::Slice(e) => {
                let e = *e;
                self.web_scalar(e)
            }
            _ => self.web_scalar(t),
        }
    }

    fn is_web_struct(&self, t: TypeId, name: &str) -> bool {
        self.struct_of(t).is_some_and(|s| self.structs[s as usize].full_name == format!("std.web.{name}")) && matches!(self.tt.get(t), Type::Pointer(_))
    }

    /// `web.Result<X>` -> X.
    fn web_result_arg(&self, t: TypeId) -> Option<TypeId> {
        let Type::Struct(d) = *self.tt.get(t) else { return None };
        let (tm, args) = self.gen.inst_structs.get(&d.0)?;
        let tm = &self.gen.templates[*tm as usize];
        (tm.is_std && tm.name == "Result" && self.nss[tm.ns].path == "std.web").then(|| args[0])
    }

    /// Splits a handler result into (shape, fallible).
    fn web_ret(&self, ret: TypeId) -> (Ret, bool) {
        let (inner, fallible) = match self.tt.fallible_inner(ret) {
            Some(i) => (i, true),
            None => (ret, false),
        };
        let shape = if matches!(self.tt.get(inner), Type::Void) {
            Ret::Void
        } else if let Some(x) = self.web_result_arg(inner) {
            Ret::Result(x)
        } else {
            Ret::Value(inner)
        };
        (shape, fallible)
    }

    /// Whether a type is provided by the framework or the stdlib (not
    /// checked for registration at compile time).
    fn web_std_type(&self, t: TypeId) -> bool {
        let t = match self.tt.get(t) {
            Type::Pointer(x) => *x,
            _ => t,
        };
        match self.tt.get(t) {
            Type::Struct(d) => self.structs[d.0 as usize].full_name.starts_with("std."),
            Type::Interface(d) => self.interfaces[d.0 as usize].full_name.starts_with("std."),
            _ => true,
        }
    }

    // -- @validate checks ---------------------------------------------------------------------

    /// Reports `@validate` rules that cannot apply to the type they are on
    /// (minLength on an int, min on a string, email on a number...).
    fn web_check_rules(&mut self, a: &hir::Attr, ty: TypeId, what: &str) {
        let mut t = ty;
        while let Type::Pointer(x) = self.tt.get(t) {
            t = *x;
        }
        let numeric = matches!(self.tt.get(t), Type::Int(_) | Type::Float32 | Type::Float64);
        let string = matches!(self.tt.get(t), Type::String);
        let sized = string || matches!(self.tt.get(t), Type::Slice(_) | Type::Map(..));
        let tn = self.tn(ty);
        for (k, v) in &a.args {
            let set = match (k.as_str(), v) {
                ("min", ConstValue::Float(f)) => *f > -1.0e308,
                ("max", ConstValue::Float(f)) => *f < 1.0e308,
                ("minLength" | "maxLength", ConstValue::Int(i)) => *i >= 0,
                ("pattern", ConstValue::Str(p)) => !p.is_empty(),
                ("email", ConstValue::Bool(b)) => *b,
                ("range", ConstValue::List(l)) => {
                    if !l.is_empty() && l.len() != 2 {
                        self.err(format!("@validate on {what}: range needs two values [min, max]"), a.span);
                    }
                    !l.is_empty()
                }
                _ => false,
            };
            if !set {
                continue;
            }
            let ok = match k.as_str() {
                "min" | "max" | "range" => numeric,
                "minLength" | "maxLength" => sized,
                "pattern" | "email" => string,
                _ => true,
            };
            if !ok {
                let applies = match k.as_str() {
                    "min" | "max" | "range" => "numbers",
                    "minLength" | "maxLength" => "strings, slices and maps",
                    _ => "strings",
                };
                self.err(format!("@validate({k}) on {what} of type {tn}: {k} applies to {applies}"), a.span);
            }
        }
    }

    /// Checks the `@validate` attributes of the fields of `ty` (a body
    /// type), recursively through structs, pointers and slices.
    fn web_check_struct_rules(&mut self, ty: TypeId, seen: &mut Vec<u32>) {
        let t = match self.tt.get(ty) {
            Type::Pointer(x) | Type::Slice(x) | Type::Array(x, _) => *x,
            Type::Map(_, v) => *v,
            _ => ty,
        };
        if t != ty {
            return self.web_check_struct_rules(t, seen);
        }
        let Type::Struct(d) = *self.tt.get(t) else { return };
        if seen.contains(&d.0) || self.structs[d.0 as usize].full_name.starts_with("std.") {
            return;
        }
        seen.push(d.0);
        let file = self.structs[d.0 as usize].file;
        let sname = self.structs[d.0 as usize].name.clone();
        for f in self.structs[d.0 as usize].fields.clone() {
            for a in &f.attrs {
                if self.web_attr_name(a).as_deref() == Some("validate") {
                    let saved = self.cur_file;
                    self.cur_file = file;
                    self.web_check_rules(a, f.ty, &format!("field {sname}.{}", f.name));
                    self.cur_file = saved;
                }
            }
            self.web_check_struct_rules(f.ty, seen);
        }
    }

    // -- parameters ------------------------------------------------------------------------

    /// Classifies a handler parameter; reports an error (and returns
    /// `None`) for one that cannot be bound.
    fn web_classify(&mut self, pname: &str, ty: TypeId, attrs: &[hir::Attr], afile: usize, route_params: &[String], verb: &str, who: &str, span: Span) -> Option<PInfo> {
        let mut src = None;
        let mut validate = None;
        let mut required = false;
        for a in attrs {
            let Some(n) = self.web_attr_name(a) else { continue };
            let rename = |s: String| if s.is_empty() { pname.to_string() } else { s };
            match n.as_str() {
                "query" => src = Some(Src::Query(rename(Self::attr_str(a, "name")))),
                "header" => src = Some(Src::Header(rename(Self::attr_str(a, "name")))),
                "form" => src = Some(Src::Form(rename(Self::attr_str(a, "name")))),
                "body" => src = Some(Src::Body),
                "fromServices" => src = Some(Src::Service),
                "validate" => {
                    validate = Some(Self::validate_src(a));
                    required = Self::attr_required(a);
                }
                _ => {}
            }
        }
        let tn = self.tn(ty);
        let src = match src {
            Some(s) => s,
            None if self.is_web_struct(ty, "Context") => Src::Context,
            None if route_params.iter().any(|p| p == pname) => Src::Path,
            None if matches!(self.tt.get(ty), Type::Interface(_)) => Src::Service,
            None if matches!(self.tt.get(ty), Type::Pointer(x) if matches!(self.tt.get(*x), Type::Struct(_))) => Src::Service,
            None if self.web_text_bindable(ty) => Src::Query(pname.to_string()),
            None if matches!(self.tt.get(ty), Type::Struct(_) | Type::Slice(_) | Type::Map(..)) => {
                if verb == "GET" || verb == "DELETE" {
                    self.err(format!("{who}: parameter {pname} of type {tn} would be the request body, but {verb} requests have none (bind it with @query fields or use POST/PUT/PATCH)"), span);
                    return None;
                }
                Src::Body
            }
            None => {
                self.err(format!("{who}: cannot bind parameter {pname} of type {tn} (route parameters, query and header values must be strings, numbers, bools or plain enums)"), span);
                return None;
            }
        };
        match &src {
            Src::Path if !route_params.iter().any(|p| p == pname) => {
                self.err(format!("{who}: parameter {pname} is not a parameter of the route (no {{{pname}}} segment)"), span);
                return None;
            }
            Src::Path if !self.web_scalar(ty) => {
                self.err(format!("{who}: route parameter {{{pname}}} has type {tn}; route parameters must be strings, numbers, bools or plain enums"), span);
                return None;
            }
            Src::Query(_) | Src::Header(_) | Src::Form(_) if !self.web_text_bindable(ty) => {
                self.err(format!("{who}: parameter {pname} of type {tn} cannot be bound from text (use strings, numbers, bools, plain enums or slices of them)"), span);
                return None;
            }
            Src::Service if !matches!(self.tt.get(ty), Type::Interface(_) | Type::Pointer(_)) => {
                self.err(format!("{who}: service parameter {pname} must be a pointer or an interface, not {tn}"), span);
                return None;
            }
            _ => {}
        }
        if let Some(a) = attrs.iter().find(|a| self.web_attr_name(a).as_deref() == Some("validate")) {
            let saved = self.cur_file;
            self.cur_file = afile;
            self.web_check_rules(a, ty, &format!("parameter {pname}"));
            self.cur_file = saved;
        }
        if src == Src::Body {
            self.web_check_struct_rules(ty, &mut Vec::new());
        }
        Some(PInfo { name: pname.to_string(), ty, src, validate, required })
    }

    /// Generated statements binding `ps` into `__p0..`; appends OpenAPI
    /// statements to `docs`.
    fn web_bind_src(&mut self, ps: &[PInfo], tys: &mut Tys, scope: &str, docs: &mut String) -> String {
        let mut out = String::new();
        for (i, p) in ps.iter().enumerate() {
            let t = tys.p(p.ty);
            let v = format!("__p{i}");
            let (bind, present, doc_in) = match &p.src {
                Src::Path => (format!("bindPath<{t}>(__c, {})", lit(&p.name)), "true".to_string(), Some(("path", p.name.clone()))),
                Src::Query(n) => (format!("bindQuery<{t}>(__c, {}, false)", lit(n)), format!("hasQueryKey(__c.Request.Query, {})", lit(n)), Some(("query", n.clone()))),
                Src::Header(n) => (format!("bindHeader<{t}>(__c, {}, false)", lit(n)), format!("__c.Request.Header({}) != \"\"", lit(n)), Some(("header", n.clone()))),
                Src::Form(n) => (format!("bindForm<{t}>(__c, {}, false)", lit(n)), format!("hasQueryKey(__c.Request.Body, {})", lit(n)), Some(("form", n.clone()))),
                Src::Body => {
                    docs.push_str(&format!("    __e.request(\"application/json\", std.encoding.json.Schema<{t}>())\n"));
                    (format!("bindBody<{t}>(__c)"), "true".into(), None)
                }
                Src::Service => (format!("resolve<{t}>({scope})?"), "true".into(), None),
                Src::Context => ("__c".into(), "true".into(), None),
            };
            out.push_str(&format!("        var {v} = {bind}\n"));
            if let Some(val) = &p.validate {
                if !matches!(p.src, Src::Body | Src::Service | Src::Context) {
                    out.push_str(&format!("        checkParam<{t}>(__c, {}, {v}, {val}, {present})\n", lit(&p.name)));
                }
            }
            if let Some((loc, n)) = doc_in {
                if loc == "form" {
                    docs.push_str(&format!("    __e.param({}, \"form\", {}, std.encoding.json.Schema<{t}>())\n", lit(&n), p.required));
                } else {
                    docs.push_str(&format!("    __e.param({}, {}, {}, std.encoding.json.Schema<{t}>())\n", lit(&n), lit(loc), loc == "path" || p.required));
                }
            }
        }
        out
    }

    /// The statements after the call: the response, and its OpenAPI entry.
    fn web_result_src(&mut self, ret: &Ret, verb: &str, tys: &mut Tys, docs: &mut String) -> String {
        match ret {
            Ret::Void => {
                docs.push_str("    __e.response(204, \"\", \"\")\n");
                "        writeVoid(__c)\n".into()
            }
            Ret::Result(x) => {
                let t = tys.p(*x);
                let status = if verb == "POST" { 201 } else { 200 };
                let ct = if matches!(self.tt.get(*x), Type::String) { "text/plain" } else { "application/json" };
                docs.push_str(&format!("    __e.response({status}, {}, std.encoding.json.Schema<{t}>())\n", lit(ct)));
                "        __r.writeTo(__c)\n".into()
            }
            Ret::Value(x) => {
                let t = tys.p(*x);
                let ct = if matches!(self.tt.get(*x), Type::String) { "text/plain" } else { "application/json" };
                docs.push_str(&format!("    __e.response(200, {}, std.encoding.json.Schema<{t}>())\n", lit(ct)));
                format!("        writeValue<{t}>(__c, 200, __r)\n")
            }
        }
    }

    // -- construction (controllers and services) ----------------------------------------------

    /// The static `New` of struct `sid`, if any.
    fn web_ctor(&self, sid: u32) -> Option<FuncId> {
        self.structs[sid as usize].methods.iter().find(|(n, f)| n == "New" && matches!(self.funcs[f.0 as usize].kind, hir::FuncKind::Method { is_static: true, .. })).map(|x| x.1)
    }

    /// Statements leaving a `*S` in `out`, built in scope expression
    /// `scope`: `S.New(deps...)` or the zero S with injected fields
    /// (`all`: every pointer/interface field, as for controllers). Returns
    /// the statements and the dependency types.
    fn web_construct(&mut self, sty: TypeId, scope: &str, out: &str, all: bool, who: &str, span: Span) -> Option<(String, Vec<TypeId>)> {
        let Type::Struct(d) = *self.tt.get(sty) else {
            let tn = self.tn(sty);
            self.err(format!("{who}: {tn} is not a struct"), span);
            return None;
        };
        let sid = d.0;
        if let Some(ctor) = self.web_ctor(sid) {
            let sig = self.sigs[ctor.0 as usize].clone();
            if sig.is_async {
                self.err(format!("{who}: {}.New must not be async to be used by the container", self.structs[sid as usize].name), span);
                return None;
            }
            let mut deps = Vec::new();
            for p in &sig.params {
                if !matches!(self.tt.get(*p), Type::Pointer(_) | Type::Interface(_)) {
                    let (sn, pn) = (self.structs[sid as usize].name.clone(), self.tn(*p));
                    self.err(format!("{who}: parameter of type {pn} of {sn}.New cannot be injected (constructor parameters must be services: pointers or interfaces)"), span);
                    return None;
                }
                deps.push(*p);
            }
            let (inner, fallible) = match self.tt.fallible_inner(sig.ret) {
                Some(i) => (i, true),
                None => (sig.ret, false),
            };
            let ptr = self.tt.pointer(sty);
            if inner != sty && inner != ptr {
                let (sn, rn) = (self.structs[sid as usize].name.clone(), self.tn(sig.ret));
                self.err(format!("{who}: {sn}.New returns {rn}; the container needs {sn} or *{sn} (optionally with !)"), span);
                return None;
            }
            // placeholders S, D0.. are replaced by the caller's type parameters
            let args: Vec<String> = (0..deps.len()).map(|i| format!("resolve<@D{i}@>({scope})?")).collect();
            let q = if fallible { "?" } else { "" };
            let call = format!("@S@.New({}){q}", args.join(", "));
            let text = if inner == sty { format!("        var {out}v = {call}\n        var {out} = &{out}v\n") } else { format!("        var {out} = {call}\n") };
            return Some((text, deps));
        }
        let mut deps = Vec::new();
        for f in self.structs[sid as usize].fields.clone() {
            let tag = f.attrs.iter().find(|a| self.web_attr_name(a).as_deref() == Some("inject"));
            if tag.is_some_and(|a| a.args.iter().any(|(k, v)| k == "skip" && *v == ConstValue::Bool(true))) {
                continue;
            }
            let ptr_like = matches!(self.tt.get(f.ty), Type::Pointer(_) | Type::Interface(_));
            if tag.is_some() || (all && ptr_like) {
                deps.push(f.ty);
            }
        }
        let text = format!("        var {out}v @S@\n        var {out} = &{out}v\n        injectInto<@S@>({scope}, {out}, {all})?\n");
        Some((text, deps))
    }

    /// Replaces the `@S@` / `@Dk@` placeholders of construction code.
    fn web_fill(text: &str, s: &str, deps: &[TypeId], tys: &mut Tys) -> String {
        let mut out = text.replace("@S@", s);
        for (i, d) in deps.iter().enumerate() {
            out = out.replace(&format!("@D{i}@"), &tys.p(*d));
        }
        out
    }

    fn web_require(&mut self, deps: &[TypeId], owner: &str, span: Span) {
        for d in deps {
            self.web.required.push((*d, owner.to_string(), span, self.cur_file));
        }
    }

    /// Records a registered service type.
    fn web_registered(&mut self, t: TypeId) {
        self.web.registered.push(t);
        if self.unit.is_some() {
            let key = self.tn_full(t);
            let origin = self.unit_origin_file();
            self.unit.as_mut().unwrap().web_registered.push((key, origin));
        }
    }

    fn web_route_check(&mut self, verb: &str, pattern: &str, segs: &[String], who: &str, span: Span) {
        let norm: Vec<String> = segs.iter().map(|s| if s.starts_with('{') { "{}".to_string() } else { s.clone() }).collect();
        let key = format!("{verb} /{}", norm.join("/"));
        if let Some(u) = self.unit.as_mut() {
            // Duplicates are found over the whole program (see units).
            u.web_routes.push(super::units::WebRoute { key, pattern: pattern.to_string(), who: who.to_string(), verb: verb.to_string(), span, file: self.cur_file });
            return;
        }
        if let Some((_, prev, _, _)) = self.web.routes.iter().find(|r| r.0 == key).cloned() {
            self.err(format!("{who}: duplicate route {verb} {pattern} (already mapped as {verb} {prev})"), span);
            return;
        }
        self.web.routes.push((key, pattern.to_string(), span, self.cur_file));
    }

    // -- controllers ----------------------------------------------------------------------------

    fn web_map_controllers(&mut self, recv: hir::Expr, targs: Option<&[ast::TypeExpr]>, args: &[ast::Expr], span: Span) -> hir::Expr {
        if !args.is_empty() {
            self.err("MapController takes no arguments: write app.MapController<C>()", span);
            self.check_args_loose(args);
            return self.invalid(span);
        }
        let Some(targs) = targs.filter(|t| !t.is_empty()) else {
            self.err("MapController needs the controller type: app.MapController<UsersController>()", span);
            return self.invalid(span);
        };
        let ctys: Vec<TypeId> = targs.iter().map(|t| self.resolve_type(t)).collect();
        if ctys.iter().any(|t| self.tt.is_invalid(*t)) {
            return self.invalid(span);
        }
        let mut tys = Tys::default();
        let mut body = String::new();
        let mut ok = true;
        for t in ctys {
            ok &= self.web_map_controller(t, span, &mut tys, &mut body);
        }
        if !ok {
            return self.invalid(span);
        }
        let n = self.web_counter();
        let src = format!("namespace std.web\n\nfunc webController{n}<{}>(__a *App) {{\n    var __e *Endpoint\n{body}    _ = __e\n}}\n", tys.generics());
        self.web_call(src, tys.0, vec![recv], span)
    }

    /// Appends the endpoints of controller `cty` to `body`; false after an error.
    fn web_map_controller(&mut self, cty: TypeId, span: Span, tys: &mut Tys, body: &mut String) -> bool {
        let cn = self.tn(cty);
        let Type::Struct(d) = *self.tt.get(cty) else {
            self.err(format!("MapController<{cn}>: a controller must be a struct"), span);
            return false;
        };
        let sid = d.0;
        let sname = self.structs[sid as usize].name.clone();
        let sattrs = self.structs[sid as usize].attrs.clone();
        let Some(route) = sattrs.iter().find(|a| self.web_attr_name(a).as_deref() == Some("route")) else {
            self.err(format!("MapController<{cn}>: {sname} has no @route(\"/prefix\") attribute"), span);
            return false;
        };
        let prefix = Self::attr_str(route, "path");
        let ctl_auth = sattrs.iter().find(|a| self.web_attr_name(a).as_deref() == Some("authorize")).map(|a| Self::attr_strs(a, "roles"));
        let tag = sname.strip_suffix("Controller").filter(|s| !s.is_empty()).unwrap_or(&sname).to_string();
        let c = tys.p(cty);
        // construction of the controller (per request)
        let Some((ctor, deps)) = self.web_construct(cty, "__c.Scope", "__ctl", true, &format!("MapController<{cn}>"), span) else { return false };
        self.web_require(&deps, &sname, span);
        for dep in &deps {
            let p = tys.p(*dep);
            body.push_str(&format!("    __a.require({}, keyOf<{p}>())\n", lit(&sname)));
        }
        let ctor = Self::web_fill(&ctor, &c, &deps, tys);
        let methods = self.structs[sid as usize].methods.clone();
        let mut actions = 0;
        let errors_before = self.web_errors();
        for (mname, fid) in methods {
            let fattrs = self.funcs[fid.0 as usize].attrs.clone();
            let verbs: Vec<(&str, &hir::Attr)> = fattrs.iter().filter_map(|a| self.web_attr_name(a).and_then(|n| VERBS.iter().find(|v| v.0 == n).map(|v| (v.1, a)))).collect();
            if verbs.is_empty() {
                continue;
            }
            let who = format!("{sname}.{mname}");
            let mut fspan = self.funcs[fid.0 as usize].span;
            let ffile = self.funcs[fid.0 as usize].file;
            if ffile != self.cur_file {
                fspan = span;
            }
            if matches!(self.funcs[fid.0 as usize].kind, hir::FuncKind::Method { is_static: true, .. }) {
                self.err(format!("{who}: an action must not be static"), fspan);
                continue;
            }
            let sig = self.sigs[fid.0 as usize].clone();
            let decl = match self.func_src[fid.0 as usize] {
                FuncSrc::Method(d, _) | FuncSrc::Free(d) => d,
                FuncSrc::Synthetic => continue,
            };
            // parameter attributes (already checked: muted)
            self.gen.mute += 1;
            let pattrs: Vec<Vec<hir::Attr>> = self.in_file(ffile, |s| decl.params.iter().map(|p| s.eval_attrs(&p.attrs, "Param")).collect());
            self.gen.mute -= 1;
            let summary = fattrs.iter().find(|a| self.web_attr_name(a).as_deref() == Some("summary")).map(|a| Self::attr_str(a, "text")).unwrap_or_default();
            let auth = fattrs.iter().find(|a| self.web_attr_name(a).as_deref() == Some("authorize")).map(|a| Self::attr_strs(a, "roles")).or(ctl_auth.clone());
            let anon = fattrs.iter().any(|a| self.web_attr_name(a).as_deref() == Some("allowAnonymous"));
            for (verb, va) in verbs {
                let rel = Self::attr_str(va, "path");
                let pattern = join_route(&prefix, &rel);
                let segs = match route_segments(&pattern) {
                    Ok(s) => s,
                    Err(e) => {
                        self.err(format!("{who}: invalid route {pattern:?}: {e}"), fspan);
                        continue;
                    }
                };
                let pattern = format!("/{}", segs.join("/"));
                self.web_route_check(verb, &pattern, &segs, &who, fspan);
                let rps = param_names(&segs);
                let mut ps = Vec::new();
                let mut ok = true;
                for (i, p) in decl.params.iter().enumerate() {
                    let ty = sig.params[i];
                    match self.web_classify(&p.name.name, ty, &pattrs[i], ffile, &rps, verb, &who, fspan) {
                        Some(pi) => ps.push(pi),
                        None => ok = false,
                    }
                }
                for rp in &rps {
                    if !decl.params.iter().any(|p| p.name.name == *rp) || ps.iter().any(|p| p.name == *rp && p.src != Src::Path) {
                        self.err(format!("{who}: route {pattern} has parameter {{{rp}}} but the action has no route parameter named {rp}"), fspan);
                        ok = false;
                    }
                }
                if ps.iter().filter(|p| p.src == Src::Body).count() > 1 {
                    self.err(format!("{who}: more than one body parameter"), fspan);
                    ok = false;
                }
                if !ok {
                    continue;
                }
                let deps: Vec<TypeId> = ps.iter().filter(|p| p.src == Src::Service).map(|p| p.ty).collect();
                self.web_require(&deps, &who, span);
                let (ret, fallible) = self.web_ret(sig.ret);
                let mut docs = String::new();
                if !summary.is_empty() {
                    docs.push_str(&format!("    __e.Summary = {}\n", lit(&summary)));
                }
                if let Some(roles) = &auth {
                    let rs: Vec<String> = roles.iter().map(|r| lit(r)).collect();
                    docs.push_str(&format!("    __e.RequireAuthorization([{}])\n", rs.join(", ")));
                    if anon {
                        docs.push_str("    __e.Anonymous = true\n");
                    }
                }
                let binds = self.web_bind_src(&ps, tys, "__c.Scope", &mut docs);
                let callargs: Vec<String> = (0..ps.len()).map(|i| format!("__p{i}")).collect();
                let aw = if sig.is_async { "await " } else { "" };
                let q = if fallible { "?" } else { "" };
                let call = format!("{aw}__ctl.{mname}({}){q}", callargs.join(", "));
                let call = if matches!(ret, Ret::Void) { format!("        {call}\n") } else { format!("        var __r = {call}\n") };
                let result = self.web_result_src(&ret, verb, tys, &mut docs);
                if ps.iter().any(|p| !matches!(p.src, Src::Service | Src::Context)) {
                    docs.push_str("    __e.response(400, \"application/problem+json\", \"\")\n");
                }
                body.push_str(&format!(
                    "    __e = __a.addEndpoint({}, {}, {}, {}, async func(__c *Context) void! {{\n{binds}        if __c.failed() {{\n            return\n        }}\n{ctor}{call}{result}    }})\n{docs}",
                    lit(verb),
                    lit(&pattern),
                    lit(&who),
                    lit(&tag)
                ));
                actions += 1;
            }
        }
        let failed = self.web_errors() > errors_before;
        if actions == 0 {
            if !failed {
                self.err(format!("MapController<{cn}>: {sname} has no action methods (@get, @post, @put, @delete, @patch)"), span);
            }
            return false;
        }
        true
    }

    // -- minimal API -------------------------------------------------------------------------

    fn web_map_minimal(&mut self, recv: hir::Expr, verb: &str, args: &[ast::Expr], span: Span) -> hir::Expr {
        let who = format!("Map{}{}", &verb[..1], verb[1..].to_lowercase());
        if args.len() != 2 {
            self.err(format!("{who} takes a route and a handler: app.{who}(\"/path/{{id}}\", (id int) => ...)"), span);
            self.check_args_loose(args);
            return self.invalid(span);
        }
        let string = self.tt.string;
        let pattern = match self.fold(&args[0]).map(|f| f.value) {
            Some(ConstValue::Str(s)) => s,
            _ => {
                self.err(format!("{who}: the route must be a string constant (it is checked at compile time)"), args[0].span);
                self.check_args_loose(args);
                return self.invalid(span);
            }
        };
        let path_e = self.expr_to(&args[0], string, Ctx::Arg(0, who.clone()));
        let segs = match route_segments(&pattern) {
            Ok(s) => s,
            Err(e) => {
                self.err(format!("{who}: invalid route {pattern:?}: {e}"), args[0].span);
                self.expr(&args[1], None);
                return self.invalid(span);
            }
        };
        let norm = format!("/{}", segs.join("/"));
        self.web_route_check(verb, &norm, &segs, &who, args[0].span);
        let rps = param_names(&segs);
        // parameter names and attributes from the lambda or function
        let hast = expr::strip_parens(&args[1]);
        let h = self.value(&args[1], None);
        if self.tt.is_invalid(h.ty) {
            return self.invalid(span);
        }
        let Type::Func(fsig) = self.tt.get(h.ty).clone() else {
            let tn = self.tn(h.ty);
            self.err(format!("{who}: the handler must be a function, not {tn}"), args[1].span);
            return self.invalid(span);
        };
        let afile = match &h.kind {
            ExprKind::FuncRef(f) => self.funcs[f.0 as usize].file,
            _ => self.cur_file,
        };
        let names_attrs: Vec<(String, Vec<hir::Attr>)> = match &hast.kind {
            ast::ExprKind::Lambda { params, .. } => params.iter().map(|p| (p.name.name.clone(), self.eval_attrs(&p.attrs, "Param"))).collect(),
            _ => match &h.kind {
                ExprKind::FuncRef(f) => match self.func_src[f.0 as usize] {
                    FuncSrc::Free(d) | FuncSrc::Method(d, _) => {
                        let file = self.funcs[f.0 as usize].file;
                        self.gen.mute += 1;
                        let v = self.in_file(file, |s| d.params.iter().map(|p| (p.name.name.clone(), s.eval_attrs(&p.attrs, "Param"))).collect());
                        self.gen.mute -= 1;
                        v
                    }
                    FuncSrc::Synthetic => Vec::new(),
                },
                _ => Vec::new(),
            },
        };
        if names_attrs.len() != fsig.params.len() {
            self.err(format!("{who}: the handler must be a lambda or a named function (its parameter names are bound to the route)"), args[1].span);
            return self.invalid(span);
        }
        let hspan = args[1].span;
        let mut ps = Vec::new();
        let mut ok = true;
        for (i, (n, attrs)) in names_attrs.iter().enumerate() {
            match self.web_classify(n, fsig.params[i], attrs, afile, &rps, verb, &who, hspan) {
                Some(p) => ps.push(p),
                None => ok = false,
            }
        }
        for rp in &rps {
            if !names_attrs.iter().any(|(n, _)| n == rp) || ps.iter().any(|p| p.name == *rp && p.src != Src::Path) {
                self.err(format!("{who}: route {pattern} has parameter {{{rp}}} but the handler has no route parameter named {rp}"), hspan);
                ok = false;
            }
        }
        if !ok {
            return self.invalid(span);
        }
        let deps: Vec<TypeId> = ps.iter().filter(|p| p.src == Src::Service).map(|p| p.ty).collect();
        self.web_require(&deps, &format!("{verb} {norm}"), span);
        let (rty, is_async) = match self.tt.get(fsig.ret) {
            Type::Task(x) => (*x, true),
            _ => (fsig.ret, false),
        };
        let (ret, fallible) = self.web_ret(rty);
        let mut tys = Tys::default();
        let f = tys.p(h.ty);
        let mut docs = String::new();
        let binds = self.web_bind_src(&ps, &mut tys, "__c.Scope", &mut docs);
        let callargs: Vec<String> = (0..ps.len()).map(|i| format!("__p{i}")).collect();
        let aw = if is_async { "await " } else { "" };
        let q = if fallible { "?" } else { "" };
        let call = format!("{aw}__f({}){q}", callargs.join(", "));
        let call = if matches!(ret, Ret::Void) { format!("        {call}\n") } else { format!("        var __r = {call}\n") };
        let result = self.web_result_src(&ret, verb, &mut tys, &mut docs);
        if ps.iter().any(|p| !matches!(p.src, Src::Service | Src::Context)) {
            docs.push_str("    __e.response(400, \"application/problem+json\", \"\")\n");
        }
        let mut reqs = String::new();
        for d in &deps {
            let p = tys.p(*d);
            reqs.push_str(&format!("    __a.require({}, keyOf<{p}>())\n", lit(&format!("{verb} {norm}"))));
        }
        let n = self.web_counter();
        let src = format!(
            "namespace std.web\n\nfunc webMinimal{n}<{}>(__a *App, __m string, __path string, __f {f}) *Endpoint {{\n{reqs}    var __e = __a.addEndpoint(__m, __path, \"\", \"\", async func(__c *Context) void! {{\n{binds}        if __c.failed() {{\n            return\n        }}\n{call}{result}    }})\n{docs}    return __e\n}}\n",
            tys.generics()
        );
        let m = mk(ExprKind::Const(ConstValue::Str(verb.to_string())), string, span);
        self.web_call(src, tys.0, vec![recv, m, path_e, h], span)
    }

    // -- services ---------------------------------------------------------------------------

    fn web_add_service(&mut self, recv: hir::Expr, lifetime: &str, targs: Option<&[ast::TypeExpr]>, args: &[ast::Expr], span: Span) -> hir::Expr {
        let who = format!("Add{lifetime}");
        if !args.is_empty() {
            self.err(format!("{who}<T>() takes no arguments (use Add{lifetime}Factory<T>(f) for a factory, AddInstance(x) for a value)"), span);
            self.check_args_loose(args);
            return self.invalid(span);
        }
        let ts: Vec<TypeId> = targs.unwrap_or(&[]).iter().map(|t| self.resolve_type(t)).collect();
        if ts.iter().any(|t| self.tt.is_invalid(*t)) {
            return self.invalid(span);
        }
        let (key, impl_) = match ts.as_slice() {
            [t] => (None, *t),
            [i, t] => (Some(*i), *t),
            _ => {
                self.err(format!("{who} needs one type argument (Add{lifetime}<UserService>()) or an interface and its implementation (Add{lifetime}<Store, SqlStore>())"), span);
                return self.invalid(span);
            }
        };
        let iname = self.tn(impl_);
        if !matches!(self.tt.get(impl_), Type::Struct(_)) {
            self.err(format!("{who}<{iname}>: the service type must be a struct (it is registered as *{iname})"), span);
            return self.invalid(span);
        }
        let ptr = self.tt.pointer(impl_);
        if let Some(i) = key {
            if !matches!(self.tt.get(i), Type::Interface(_)) {
                let n = self.tn(i);
                self.err(format!("{who}<{n}, {iname}>: the first type argument must be an interface"), span);
                return self.invalid(span);
            }
            self.web_registered(i);
        } else {
            self.web_registered(ptr);
        }
        let Some((ctor, deps)) = self.web_construct(impl_, "__sc", "__v", false, &format!("{who}<{iname}>"), span) else { return self.invalid(span) };
        let owner = format!("*{}", self.tn(impl_));
        self.web_require(&deps, &owner, span);
        let mut tys = Tys::default();
        let s = tys.p(impl_);
        let ctor = Self::web_fill(&ctor, &s, &deps, &mut tys);
        let (kt, conv) = match key {
            Some(i) => {
                let p = tys.p(i);
                (p.clone(), format!("        var __i {p} = __v\n        return wrap<{p}>(__i)\n"))
            }
            None => {
                let p = tys.p(ptr);
                (p.clone(), format!("        return wrap<{p}>(__v)\n"))
            }
        };
        let deplist: Vec<String> = deps.iter().map(|d| format!("keyOf<{}>()", tys.p(*d))).collect();
        let n = self.web_counter();
        let src = format!(
            "namespace std.web\n\nfunc webService{n}<{}>(__s *Services) *Services {{\n    return __s.register(keyOf<{kt}>(), Lifetime.{lifetime}, func(__sc *Scope) any! {{\n{ctor}{conv}    }}, [{}])\n}}\n",
            tys.generics(),
            deplist.join(", ")
        );
        self.web_call(src, tys.0, vec![recv], span)
    }

    // -- end of analysis --------------------------------------------------------------------------

    /// Compile-time DI check: a dependency of a mapped controller, handler
    /// or registered service whose type no `Add*` call of the program
    /// registers is an error (types of the stdlib are provided or checked
    /// by `App.Build` at run time).
    pub(crate) fn web_finish(&mut self) {
        if self.unit.is_some() {
            self.web_finish_unit();
            return;
        }
        let required = std::mem::take(&mut self.web.required);
        let mut reported: Vec<(TypeId, String)> = Vec::new();
        for (t, owner, span, file) in required {
            if self.web.registered.contains(&t) || self.web_std_type(t) || reported.contains(&(t, owner.clone())) {
                continue;
            }
            reported.push((t, owner.clone()));
            let tn = self.tn(t);
            let hint = match self.tt.get(t) {
                Type::Pointer(x) => {
                    let x = *x;
                    format!("app.Services.AddSingleton<{}>() (or AddScoped / AddTransient / AddInstance)", self.tn(x))
                }
                _ => format!("app.Services.AddSingleton<{tn}, Impl>() or AddInstance"),
            };
            let saved = self.cur_file;
            self.cur_file = file;
            self.err(format!("{owner} needs service {tn}, which is never registered: add {hint}"), span);
            self.cur_file = saved;
        }
    }

    /// [`Sema::web_finish`] in unit mode: the dependencies are checked by
    /// the caller against the registrations of the whole program; type
    /// names are spelled from the namespace of the file needing them.
    fn web_finish_unit(&mut self) {
        let required = std::mem::take(&mut self.web.required);
        let saved_ns = self.cur_ns;
        for (t, owner, span, file) in required {
            if self.web_std_type(t) {
                continue;
            }
            if let Some(fs) = self.file_scopes.get(file) {
                self.cur_ns = fs.ns;
            }
            let tn = self.tn(t);
            let hint = match self.tt.get(t) {
                Type::Pointer(x) => {
                    let x = *x;
                    format!("app.Services.AddSingleton<{}>() (or AddScoped / AddTransient / AddInstance)", self.tn(x))
                }
                _ => format!("app.Services.AddSingleton<{tn}, Impl>() or AddInstance"),
            };
            let diag = Diagnostic::error(format!("{owner} needs service {tn}, which is never registered: add {hint}"), span).in_file(file);
            let key = self.tn_full(t);
            self.unit.as_mut().unwrap().web_required.push(super::units::WebRequired { key, owner, diag });
        }
        self.cur_ns = saved_ns;
    }
}
