//! Namespaces, declarations, imports, name lookup and type resolution.

use super::*;
use crate::hir::{FuncKind, NamespaceId};

/// Result of resolving a dotted path symbolically.
#[derive(Clone, Copy, Debug)]
pub(crate) enum PathRes {
    Member(Member),
    Builtin(BuiltinFn),
    BuiltinType(TypeId),
    /// Enum variant (enum id, variant index).
    Variant(u32, u32),
    /// Static method of a struct.
    StaticMethod(FuncId),
}

/// Embedding depth after which promotion lookup gives up (guards cycles).
const MAX_EMBED_DEPTH: usize = 16;

/// Builtin attribute names.
pub(crate) const BUILTIN_ATTRS: &[&str] = &["attribute", "reflect", "deprecated", "inline", "test", "json"];

impl<'a> Sema<'a> {
    // -- namespace tree ------------------------------------------------------

    /// Returns the namespace for `path`, creating it and its parents.
    pub(crate) fn ensure_ns(&mut self, path: &str, span: Span) -> NsIdx {
        if let Some(&i) = self.ns_index.get(path) {
            return i;
        }
        let is_std = path == "std" || path.starts_with("std.");
        let parent = path.rfind('.').map(|p| self.ensure_ns(&path[..p], span));
        let idx = self.nss.len();
        let hir_id = if is_std {
            None
        } else {
            Some(NamespaceId(self.nss.iter().filter(|n| n.hir_id.is_some()).count() as u32))
        };
        self.nss.push(Ns { path: path.to_string(), members: HashMap::new(), is_std, hir_id, attrs: Vec::new() });
        self.ns_index.insert(path.to_string(), idx);
        let last = path.rsplit('.').next().unwrap().to_string();
        match parent {
            Some(p) => {
                self.nss[p].members.insert(last, (Member::Ns(idx), span, self.cur_file));
            }
            None => {
                self.roots.insert(last, idx);
            }
        }
        idx
    }

    pub(crate) fn build_stdlib(&mut self) {
        for ns in stdlib::NAMESPACES {
            self.ensure_ns(ns, Span::DUMMY);
        }
        for (i, f) in stdlib::FUNCS.iter().enumerate() {
            let ns = self.ns_index[f.ns];
            self.nss[ns].members.insert(f.name.to_string(), (Member::StdFunc(i), Span::DUMMY, 0));
        }
        for (ns, name, sp) in stdlib::SPECIALS {
            let ns = self.ns_index[*ns];
            self.nss[ns].members.insert(name.to_string(), (Member::Special(*sp), Span::DUMMY, 0));
        }
        for (i, (ns, name, _)) in stdlib::CONSTS.iter().enumerate() {
            let ns = self.ns_index[*ns];
            self.nss[ns].members.insert(name.to_string(), (Member::StdConst(i), Span::DUMMY, 0));
        }
    }

    /// Adds a member to a namespace, reporting duplicates.
    fn register(&mut self, ns: NsIdx, name: &ast::Ident, m: Member) {
        if let Some((_, prev, pfile)) = self.nss[ns].members.get(&name.name).cloned() {
            let note_span = if pfile == self.cur_file { Some(prev) } else { None };
            let path = self.nss[ns].path.clone();
            self.err_note(format!("duplicate declaration of {} in namespace {path}", name.name), name.span, "previously declared here", note_span);
            return;
        }
        self.nss[ns].members.insert(name.name.clone(), (m, name.span, self.cur_file));
    }

    fn reject_generics(&mut self, g: &[ast::GenericParam]) {
        if let Some(p) = g.first() {
            self.err("generics are not supported yet", p.span);
        }
    }

    /// Registers every declaration of every file.
    pub(crate) fn collect_decls(&mut self) {
        let files = self.files;
        for (fi, file) in files.iter().enumerate() {
            self.cur_file = fi;
            let ns = self.ensure_ns(&file.namespace.to_string(), file.namespace.span);
            if self.nss[ns].is_std {
                self.err("user code cannot declare a std namespace", file.namespace.span);
            }
            self.cur_ns = ns;
            self.file_scopes.push(FileScope { ns, imports: Vec::new() });
            let hns = self.cur_hir_ns();
            for d in &file.decls {
                match d {
                    ast::Decl::Struct(s) => {
                        self.reject_generics(&s.generics);
                        let id = self.structs.len() as u32;
                        let full_name = format!("{}.{}", self.nss[ns].path, s.name.name);
                        let is_attribute = s.attrs.iter().any(|a| a.path.to_string() == "attribute");
                        self.structs.push(hir::Struct {
                            name: s.name.name.clone(),
                            full_name,
                            namespace: hns,
                            fields: Vec::new(),
                            methods: Vec::new(),
                            attrs: Vec::new(),
                            is_attribute,
                            exported: is_exported(&s.name.name),
                            file: fi,
                            span: s.span,
                        });
                        self.struct_src.push(s);
                        self.register(ns, &s.name, Member::Struct(id));
                        for m in &s.methods {
                            self.reject_generics(&m.generics);
                            if self.structs[id as usize].methods.iter().any(|(n, _)| *n == m.name.name) {
                                self.err(format!("duplicate method {} in struct {}", m.name.name, s.name.name), m.name.span);
                                continue;
                            }
                            let kind = FuncKind::Method { struct_id: id, is_static: m.is_static };
                            let fid = self.new_func(m, kind, FuncSrc::Method(m, id), &format!("{}.{}", s.name.name, m.name.name));
                            self.structs[id as usize].methods.push((m.name.name.clone(), fid));
                        }
                    }
                    ast::Decl::Interface(it) => {
                        self.reject_generics(&it.generics);
                        let id = self.interfaces.len() as u32;
                        self.interfaces.push(hir::Interface {
                            name: it.name.name.clone(),
                            full_name: format!("{}.{}", self.nss[ns].path, it.name.name),
                            namespace: hns,
                            methods: Vec::new(),
                            attrs: Vec::new(),
                            exported: is_exported(&it.name.name),
                            file: fi,
                            span: it.span,
                        });
                        self.iface_src.push(it);
                        self.register(ns, &it.name, Member::Interface(id));
                    }
                    ast::Decl::Enum(e) => {
                        let id = self.enums.len() as u32;
                        self.enums.push(hir::Enum {
                            name: e.name.name.clone(),
                            full_name: format!("{}.{}", self.nss[ns].path, e.name.name),
                            namespace: hns,
                            variants: Vec::new(),
                            is_plain: e.variants.iter().all(|v| v.fields.is_empty()),
                            attrs: Vec::new(),
                            exported: is_exported(&e.name.name),
                            file: fi,
                            span: e.span,
                        });
                        self.enum_src.push(e);
                        self.register(ns, &e.name, Member::Enum(id));
                    }
                    ast::Decl::Func(f) => {
                        self.reject_generics(&f.generics);
                        if f.body.is_none() {
                            self.err(format!("function {} has no body", f.name.name), f.name.span);
                        }
                        let fid = self.new_func(f, FuncKind::Free, FuncSrc::Free(f), &f.name.name);
                        self.register(ns, &f.name, Member::Func(fid));
                    }
                    ast::Decl::Var(v) => {
                        if v.names.len() != 1 {
                            self.err("tuple destructuring is not allowed at the top level", v.span);
                            continue;
                        }
                        let gid = self.new_global(&v.names[0], false, GlobalSrc::Var(v), v.span);
                        self.register(ns, &v.names[0], Member::Global(gid));
                    }
                    ast::Decl::Const(c) => {
                        let gid = self.new_global(&c.name, true, GlobalSrc::Const(c), c.span);
                        self.register(ns, &c.name, Member::Global(gid));
                    }
                }
            }
        }
    }

    fn new_func(&mut self, d: &'a ast::FuncDecl, kind: FuncKind, src: FuncSrc<'a>, local_name: &str) -> FuncId {
        let fid = FuncId(self.funcs.len() as u32);
        let mut f = self.dummy_function(d.name.name.clone(), kind, self.cur_hir_ns());
        f.full_name = format!("{}.{local_name}", self.nss[self.cur_ns].path);
        f.symbol = format!("slang_{}", f.full_name.replace('.', "__"));
        f.is_async = d.is_async;
        f.exported = is_exported(&d.name.name);
        f.span = d.span;
        self.funcs.push(f);
        self.func_src.push(src);
        self.sigs.push(Sig { params: Vec::new(), ret: self.tt.void, is_async: d.is_async });
        fid
    }

    fn new_global(&mut self, name: &ast::Ident, is_const: bool, src: GlobalSrc<'a>, span: Span) -> GlobalId {
        let gid = GlobalId(self.globals.len() as u32);
        let full_name = format!("{}.{}", self.nss[self.cur_ns].path, name.name);
        self.globals.push(hir::Global {
            name: name.name.clone(),
            symbol: format!("slang_{}", full_name.replace('.', "__")),
            full_name,
            namespace: self.cur_hir_ns(),
            ty: self.tt.invalid,
            is_const,
            value: None,
            init: None,
            attrs: Vec::new(),
            exported: is_exported(&name.name),
            file: self.cur_file,
            span,
        });
        self.global_src.push(src);
        self.global_state.push(GState::Pending);
        gid
    }

    /// Resolves the `import` lines of every file.
    pub(crate) fn resolve_imports(&mut self) {
        let files = self.files;
        for (fi, file) in files.iter().enumerate() {
            self.cur_file = fi;
            self.cur_ns = self.file_scopes[fi].ns;
            for imp in &file.imports {
                let path = imp.path.to_string();
                let entry = if let Some(&ns) = self.ns_index.get(&path) {
                    match &imp.alias {
                        Some(a) => ImportEntry::Named(a.name.clone(), Member::Ns(ns), ns),
                        None => ImportEntry::Glob(ns),
                    }
                } else {
                    let segs = &imp.path.segments;
                    let parent = segs[..segs.len() - 1].iter().map(|s| s.name.as_str()).collect::<Vec<_>>().join(".");
                    let last = segs.last().unwrap();
                    let found = self.ns_index.get(&parent).copied().and_then(|ns| self.member_of(ns, last, true).map(|m| (m, ns)));
                    match found {
                        Some((m, ns)) => {
                            let name = imp.alias.as_ref().unwrap_or(last).name.clone();
                            ImportEntry::Named(name, m, ns)
                        }
                        None => {
                            self.err(format!("unknown namespace or declaration {path} in import"), imp.path.span);
                            continue;
                        }
                    }
                };
                self.file_scopes[fi].imports.push(entry);
            }
        }
    }

    // -- lookup -------------------------------------------------------------

    /// Member `name` of namespace `ns`, checking visibility from the current
    /// namespace.
    pub(crate) fn member_of(&mut self, ns: NsIdx, name: &ast::Ident, report: bool) -> Option<Member> {
        let (m, _, _) = *self.nss[ns].members.get(&name.name)?;
        if report && ns != self.cur_ns && !self.nss[ns].is_std && !is_exported(&name.name) && !matches!(m, Member::Ns(_)) {
            let path = self.nss[ns].path.clone();
            self.err(format!("{} is private to namespace {path} (only capitalised names are exported)", name.name), name.span);
        }
        Some(m)
    }

    /// Looks up a non-local name: own namespace, imports, builtins, roots.
    pub(crate) fn lookup_global(&mut self, name: &ast::Ident, report: bool) -> Option<PathRes> {
        let n = name.name.as_str();
        if let Some((m, _, _)) = self.nss[self.cur_ns].members.get(n) {
            return Some(PathRes::Member(*m));
        }
        let mut found: Vec<(Member, NsIdx)> = Vec::new();
        let mut private: Option<NsIdx> = None;
        for e in &self.file_scopes[self.cur_file].imports {
            match e {
                ImportEntry::Glob(ns) => {
                    if let Some((m, _, _)) = self.nss[*ns].members.get(n) {
                        if self.nss[*ns].is_std || is_exported(n) || matches!(m, Member::Ns(_)) {
                            if !found.iter().any(|(f, _)| f == m) {
                                found.push((*m, *ns));
                            }
                        } else {
                            private = Some(*ns);
                        }
                    }
                }
                ImportEntry::Named(alias, m, ns) if alias == n => {
                    if !found.iter().any(|(f, _)| f == m) {
                        found.push((*m, *ns));
                    }
                }
                ImportEntry::Named(..) => {}
            }
        }
        if found.len() > 1 && report {
            let a = self.nss[found[0].1].path.clone();
            let b = self.nss[found[1].1].path.clone();
            self.err(format!("ambiguous name {n}: imported from both {a} and {b}; use an alias (`as`) or the full path"), name.span);
        }
        if let Some((m, _)) = found.first() {
            return Some(PathRes::Member(*m));
        }
        let builtin = match n {
            "error" => Some(BuiltinFn::Error),
            "len" => Some(BuiltinFn::Len),
            "append" => Some(BuiltinFn::Append),
            "delete" => Some(BuiltinFn::Delete),
            "panic" => Some(BuiltinFn::Panic),
            "typeof" => Some(BuiltinFn::Typeof),
            _ => None,
        };
        if let Some(b) = builtin {
            return Some(PathRes::Builtin(b));
        }
        if let Some(t) = self.tt.builtin(n) {
            return Some(PathRes::BuiltinType(t));
        }
        if let Some(&r) = self.roots.get(n) {
            return Some(PathRes::Member(Member::Ns(r)));
        }
        if let (Some(ns), true) = (private, report) {
            let path = self.nss[ns].path.clone();
            self.err(format!("{n} is private to namespace {path} (only capitalised names are exported)"), name.span);
        }
        None
    }

    /// Resolves as many leading segments of `segs` as name declarations
    /// (namespaces, types, static methods, variants); returns the result and
    /// the number of segments consumed. `None` if the head is unknown.
    pub(crate) fn resolve_segments(&mut self, segs: &[&ast::Ident], report: bool) -> Option<(PathRes, usize)> {
        let mut cur = self.lookup_global(segs[0], report)?;
        let mut i = 1;
        while i < segs.len() {
            let seg = segs[i];
            let next = match cur {
                PathRes::Member(Member::Ns(ns)) => match self.member_of(ns, seg, report) {
                    Some(m) => PathRes::Member(m),
                    None => {
                        if report {
                            let path = self.nss[ns].path.clone();
                            self.err(format!("unknown identifier {} in namespace {path}", seg.name), seg.span);
                        }
                        return None;
                    }
                },
                PathRes::Member(Member::Struct(s)) => {
                    let found = self.structs[s as usize].methods.iter().find(|(n, _)| *n == seg.name).map(|(_, f)| *f);
                    match found {
                        Some(f) if self.is_static(f) => {
                            self.check_member_visibility(self.structs[s as usize].namespace, &seg.name, seg.span, report);
                            PathRes::StaticMethod(f)
                        }
                        _ => {
                            if report {
                                let sn = self.structs[s as usize].name.clone();
                                let what = if found.is_some() { "is not a static method of" } else { "is not a static method of struct" };
                                self.err(format!("{} {what} {sn}", seg.name), seg.span);
                            }
                            return None;
                        }
                    }
                }
                PathRes::Member(Member::Enum(e)) => match self.enums[e as usize].variants.iter().position(|v| v.name == seg.name) {
                    Some(v) => PathRes::Variant(e, v as u32),
                    None => {
                        if report {
                            let en = self.enums[e as usize].name.clone();
                            self.err(format!("enum {en} has no variant {}", seg.name), seg.span);
                        }
                        return None;
                    }
                },
                _ => break,
            };
            cur = next;
            i += 1;
        }
        Some((cur, i))
    }

    fn is_static(&self, f: FuncId) -> bool {
        matches!(self.funcs[f.0 as usize].kind, FuncKind::Method { is_static: true, .. })
    }

    /// Reports access to a lowercase member (field/method) of a type from
    /// another namespace.
    pub(crate) fn check_member_visibility(&mut self, owner: NamespaceId, name: &str, span: Span, report: bool) {
        if report && !is_exported(name) && self.nss[self.cur_ns].hir_id != Some(owner) {
            let path = self.nss[self.ns_of_hir(owner)].path.clone();
            self.err(format!("{name} is private to namespace {path} (only capitalised names are exported)"), span);
        }
    }

    // -- types ----------------------------------------------------------------

    /// Resolves a written type.
    pub(crate) fn resolve_type(&mut self, t: &ast::TypeExpr) -> TypeId {
        use ast::TypeKind as K;
        match &t.kind {
            K::Named { path, generics } => {
                if !generics.is_empty() {
                    if path.segments.len() == 1 && path.segments[0].name == "Task" && generics.len() == 1 {
                        let inner = self.resolve_type(&generics[0]);
                        return self.tt.task(inner);
                    }
                    self.err("generics are not supported yet", t.span);
                    return self.tt.invalid;
                }
                if path.segments.len() == 1 {
                    if let Some(b) = self.tt.builtin(&path.segments[0].name) {
                        return b;
                    }
                }
                let segs: Vec<&ast::Ident> = path.segments.iter().collect();
                match self.resolve_segments(&segs, true) {
                    Some((res, n)) if n == segs.len() => match self.path_res_type(res) {
                        Some(ty) => ty,
                        None => {
                            self.err(format!("{} is not a type", path.to_string()), path.span);
                            self.tt.invalid
                        }
                    },
                    Some(_) => {
                        self.err(format!("unknown type {}", path.to_string()), path.span);
                        self.tt.invalid
                    }
                    None => {
                        if !self.diags.has_errors_at(path.span, self.cur_file) {
                            self.err(format!("unknown type {}", path.to_string()), path.span);
                        }
                        self.tt.invalid
                    }
                }
            }
            K::Fallible(inner) => {
                let i = self.resolve_type(inner);
                if self.tt.is_fallible(i) {
                    self.err("a fallible type cannot be fallible again", t.span);
                }
                self.tt.fallible(i)
            }
            K::Pointer(inner) => {
                let i = self.resolve_type(inner);
                self.tt.pointer(i)
            }
            K::Slice(inner) => {
                let i = self.resolve_type(inner);
                self.tt.slice(i)
            }
            K::Array(inner, n) => {
                let i = self.resolve_type(inner);
                self.tt.array(i, *n)
            }
            K::Map(k, v) => {
                let k = self.resolve_type(k);
                let v = self.resolve_type(v);
                if !self.is_map_key(k) {
                    self.err(format!("invalid map key type {}", self.tn(k)), t.span);
                }
                self.tt.map(k, v)
            }
            K::Func { params, ret } => {
                let ps = params.iter().map(|p| self.resolve_type(p)).collect();
                let r = match ret {
                    Some(r) => self.resolve_type(r),
                    None => self.tt.void,
                };
                self.tt.func(ps, r)
            }
            K::Tuple(ts) => {
                let ts = ts.iter().map(|p| self.resolve_type(p)).collect();
                self.tt.tuple(ts)
            }
        }
    }

    /// Map keys: numbers, bool, char, string, plain enums, pointers.
    pub(crate) fn is_map_key(&self, t: TypeId) -> bool {
        match self.tt.get(t) {
            Type::Int(_) | Type::Float32 | Type::Float64 | Type::Bool | Type::Char | Type::String | Type::Pointer(_) | Type::Invalid => true,
            Type::Enum(e) => self.enums.get(e.0 as usize).is_none_or(|e| e.is_plain),
            _ => false,
        }
    }

    /// The type named by a path resolution, if it names a type.
    pub(crate) fn path_res_type(&mut self, r: PathRes) -> Option<TypeId> {
        use crate::types::DeclId;
        Some(match r {
            PathRes::BuiltinType(t) => t,
            PathRes::Builtin(BuiltinFn::Error) => self.tt.error,
            PathRes::Member(Member::Struct(s)) => self.tt.intern(Type::Struct(DeclId(s))),
            PathRes::Member(Member::Interface(i)) => self.tt.intern(Type::Interface(DeclId(i))),
            PathRes::Member(Member::Enum(e)) => self.tt.intern(Type::Enum(DeclId(e))),
            _ => return None,
        })
    }

    pub(crate) fn struct_ty(&mut self, s: u32) -> TypeId {
        self.tt.intern(Type::Struct(crate::types::DeclId(s)))
    }

    pub(crate) fn enum_ty(&mut self, e: u32) -> TypeId {
        self.tt.intern(Type::Enum(crate::types::DeclId(e)))
    }

    // -- signatures -------------------------------------------------------

    /// Resolves struct fields, enum payloads, interface methods and
    /// function signatures.
    pub(crate) fn resolve_signatures(&mut self) {
        for sid in 0..self.structs.len() {
            let src = self.struct_src[sid];
            self.cur_file = self.structs[sid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            let mut fields: Vec<hir::StructField> = Vec::new();
            for e in &src.embeds {
                let ty = self.resolve_type(e);
                let name = match (&e.kind, self.tt.get(ty)) {
                    (ast::TypeKind::Named { path, .. }, Type::Struct(_)) => path.segments.last().unwrap().name.clone(),
                    _ => {
                        if !self.tt.is_invalid(ty) {
                            self.err("only structs can be embedded", e.span);
                        }
                        continue;
                    }
                };
                fields.push(hir::StructField { exported: is_exported(&name), name, ty, embedded: true, default: None, attrs: Vec::new(), span: e.span });
            }
            for f in &src.fields {
                let ty = self.resolve_type(&f.ty);
                if fields.iter().any(|x| x.name == f.name.name) {
                    self.err(format!("duplicate field {} in struct {}", f.name.name, src.name.name), f.name.span);
                    continue;
                }
                if f.default.is_some() && !self.structs[sid].is_attribute {
                    self.err("field default values are only allowed in @attribute structs", f.span);
                }
                fields.push(hir::StructField {
                    name: f.name.name.clone(),
                    ty,
                    embedded: false,
                    exported: is_exported(&f.name.name),
                    default: None,
                    attrs: Vec::new(),
                    span: f.span,
                });
            }
            let methods: Vec<String> = self.structs[sid].methods.iter().map(|m| m.0.clone()).collect();
            for name in &methods {
                if let Some(f) = fields.iter().find(|f| f.name == *name) {
                    let span = f.span;
                    self.err(format!("struct {} has both a field and a method named {name}", src.name.name), span);
                }
            }
            self.structs[sid].fields = fields;
        }
        self.check_recursive_structs();
        for eid in 0..self.enums.len() {
            let src = self.enum_src[eid];
            self.cur_file = self.enums[eid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            let mut vs: Vec<hir::Variant> = Vec::new();
            for v in &src.variants {
                if vs.iter().any(|x| x.name == v.name.name) {
                    self.err(format!("duplicate variant {} in enum {}", v.name.name, src.name.name), v.name.span);
                    continue;
                }
                let fields = v.fields.iter().map(|f| (f.name.name.clone(), self.resolve_type(&f.ty))).collect();
                vs.push(hir::Variant { name: v.name.name.clone(), fields, attrs: Vec::new(), span: v.span });
            }
            self.enums[eid].variants = vs;
        }
        for iid in 0..self.interfaces.len() {
            let src = self.iface_src[iid];
            self.cur_file = self.interfaces[iid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            let mut ms: Vec<hir::MethodSig> = Vec::new();
            for m in &src.methods {
                self.reject_generics(&m.generics);
                if ms.iter().any(|x| x.name == m.name.name) {
                    self.err(format!("duplicate method {} in interface {}", m.name.name, src.name.name), m.name.span);
                    continue;
                }
                let params = m.params.iter().map(|p| self.resolve_type(&p.ty)).collect();
                let ret = m.ret.as_ref().map(|r| self.resolve_type(r)).unwrap_or(self.tt.void);
                ms.push(hir::MethodSig { name: m.name.name.clone(), params, ret });
            }
            self.interfaces[iid].methods = ms;
        }
        for fid in 0..self.funcs.len() {
            let (decl, struct_id) = match self.func_src[fid] {
                FuncSrc::Free(d) => (d, None),
                FuncSrc::Method(d, s) => (d, Some(s)),
                FuncSrc::Synthetic => continue,
            };
            self.cur_file = self.funcs[fid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            let mut locals = Vec::new();
            let mut params = Vec::new();
            if let (Some(s), false) = (struct_id, decl.is_static) {
                let st = self.struct_ty(s);
                let ty = self.tt.pointer(st);
                locals.push(hir::Local { name: "this".into(), ty, kind: LocalKind::This, captured: false, address_taken: false, span: decl.name.span });
            }
            for p in &decl.params {
                let ty = self.resolve_type(&p.ty);
                if p.name.name != "_" && locals.iter().any(|l: &hir::Local| l.name == p.name.name) {
                    self.err(format!("duplicate parameter {}", p.name.name), p.name.span);
                }
                params.push(ty);
                locals.push(hir::Local { name: p.name.name.clone(), ty, kind: LocalKind::Param, captured: false, address_taken: false, span: p.span });
            }
            let ret = decl.ret.as_ref().map(|r| self.resolve_type(r)).unwrap_or(self.tt.void);
            let f = &mut self.funcs[fid];
            f.params = (0..locals.len()).map(|i| LocalId(i as u32)).collect();
            f.locals = locals;
            f.ret = ret;
            self.sigs[fid] = Sig { params, ret, is_async: decl.is_async };
        }
    }

    /// Reports structs that contain themselves by value.
    fn check_recursive_structs(&mut self) {
        fn contains(s: &Sema, target: u32, ty: TypeId, seen: &mut Vec<u32>) -> bool {
            match s.tt.get(ty) {
                Type::Struct(d) => {
                    if d.0 == target {
                        return true;
                    }
                    if seen.contains(&d.0) {
                        return false;
                    }
                    seen.push(d.0);
                    s.structs[d.0 as usize].fields.iter().any(|f| contains(s, target, f.ty, seen))
                }
                Type::Array(t, _) => contains(s, target, *t, seen),
                Type::Tuple(ts) => ts.iter().any(|t| contains(s, target, *t, seen)),
                Type::Enum(e) => s.enums.get(e.0 as usize).is_some_and(|e| e.variants.iter().any(|v| v.fields.iter().any(|f| contains(s, target, f.1, seen)))),
                _ => false,
            }
        }
        for sid in 0..self.structs.len() {
            let mut seen = Vec::new();
            if self.structs[sid].fields.iter().any(|f| contains(self, sid as u32, f.ty, &mut seen)) {
                self.cur_file = self.structs[sid].file;
                let name = self.structs[sid].name.clone();
                let span = self.struct_src[sid].name.span;
                self.err(format!("invalid recursive type {name}: it contains itself by value (use a pointer)"), span);
            }
        }
    }

    // -- struct members ------------------------------------------------------

    /// Path of field indices to field `name` of struct `sid`, following
    /// embedded structs (shallowest match wins).
    pub(crate) fn find_field(&self, sid: u32, name: &str) -> Option<Vec<u32>> {
        self.find_field_at(sid, name, 0)
    }

    fn find_field_at(&self, sid: u32, name: &str, depth: usize) -> Option<Vec<u32>> {
        let st = &self.structs[sid as usize];
        if let Some(i) = st.fields.iter().position(|f| f.name == name) {
            return Some(vec![i as u32]);
        }
        // Embedding cycles are reported elsewhere; stop following them.
        if depth > MAX_EMBED_DEPTH {
            return None;
        }
        let mut best: Option<Vec<u32>> = None;
        for (i, f) in st.fields.iter().enumerate().filter(|(_, f)| f.embedded) {
            if let Type::Struct(d) = self.tt.get(f.ty) {
                if let Some(mut p) = self.find_field_at(d.0, name, depth + 1) {
                    p.insert(0, i as u32);
                    if best.as_ref().is_none_or(|b| p.len() < b.len()) {
                        best = Some(p);
                    }
                }
            }
        }
        best
    }

    /// Non-static method `name` of struct `sid`, possibly promoted through
    /// embedded fields: (function, embedded field path).
    pub(crate) fn find_method(&self, sid: u32, name: &str) -> Option<(FuncId, Vec<u32>)> {
        self.find_method_at(sid, name, 0)
    }

    fn find_method_at(&self, sid: u32, name: &str, depth: usize) -> Option<(FuncId, Vec<u32>)> {
        let st = &self.structs[sid as usize];
        if let Some((_, f)) = st.methods.iter().find(|(n, _)| n == name) {
            if !self.is_static(*f) {
                return Some((*f, Vec::new()));
            }
        }
        if depth > MAX_EMBED_DEPTH {
            return None;
        }
        let mut best: Option<(FuncId, Vec<u32>)> = None;
        for (i, f) in st.fields.iter().enumerate().filter(|(_, f)| f.embedded) {
            if let Type::Struct(d) = self.tt.get(f.ty) {
                if let Some((m, mut p)) = self.find_method_at(d.0, name, depth + 1) {
                    p.insert(0, i as u32);
                    if best.as_ref().is_none_or(|b| p.len() < b.1.len()) {
                        best = Some((m, p));
                    }
                }
            }
        }
        best
    }
}
