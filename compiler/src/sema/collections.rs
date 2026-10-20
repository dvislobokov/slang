//! `std.collections`: `List<T>`, `Map<K, V>`, `Set<T>`, `Queue<T>` and
//! `Stack<T>`, written in Slang (below) as generic structs over the builtin
//! slices and maps, parsed once and registered as stdlib templates. Their
//! instances are ordinary monomorphised structs, so codegen and the runtime
//! need nothing new.
//!
//! Representation: `List<T>` wraps a `[]T`; `Map<K, V>` keeps the keys and
//! values in two slices (insertion order) plus a `map[K]int` from key to
//! position + 1; `Set<T>` is the same without values. `for x in list`,
//! `for x in set` and `for k, v in m` iterate in that order (lowered here
//! to a loop over the slices).
//!
//! Requirements beyond the struct constraints (`List<T>.Contains` and
//! `IndexOf` need `==` on `T`) are checked at the call; the method bodies
//! of instances that do not meet them are not checked or emitted.

use super::coerce::Ctx;
use super::generics::TmplDecl;
use super::*;
use crate::hir::{ExprKind, Stmt, StmtKind};
use std::sync::OnceLock;

/// The Slang source of `std.collections`.
const SOURCE: &str = r#"namespace std.collections

struct List<T> {
    items []T

    static func New() List<T> {
        return List<T>{}
    }

    func Add(x T) {
        this.items = append(this.items, x)
    }

    func Len() int {
        return len(this.items)
    }

    func Get(i int) T {
        return this.items[i]
    }

    func Set(i int, x T) {
        this.items[i] = x
    }

    func Insert(i int, x T) {
        var n = len(this.items)
        if i < 0 || i > n {
            panic("List.Insert: index out of range")
        }
        var zero T
        this.items = append(this.items, zero)
        var j = n
        for j > i {
            this.items[j] = this.items[j - 1]
            j -= 1
        }
        this.items[i] = x
    }

    func Remove(i int) {
        var n = len(this.items)
        _ = this.items[i]
        for j in i..n - 1 {
            this.items[j] = this.items[j + 1]
        }
        var zero T
        this.items[n - 1] = zero
        this.items = this.items[0..n - 1]
    }

    func IndexOf(x T) int {
        for i, y in this.items {
            if y == x {
                return i
            }
        }
        return -1
    }

    func Contains(x T) bool {
        return this.IndexOf(x) >= 0
    }

    func Clear() {
        this.items = nil
    }

    func Items() []T {
        var out []T = []
        for x in this.items {
            out = append(out, x)
        }
        return out
    }
}

struct Map<K: Comparable, V> {
    index map[K]int
    keys []K
    vals []V

    static func New() Map<K, V> {
        return Map<K, V>{}
    }

    func Set(k K, v V) {
        var i = this.index[k]
        if i > 0 {
            this.vals[i - 1] = v
            return
        }
        if this.index == nil {
            this.index = map[K]int{}
        }
        this.keys = append(this.keys, k)
        this.vals = append(this.vals, v)
        this.index[k] = len(this.keys)
    }

    func Get(k K) V {
        var i = this.index[k]
        if i == 0 {
            var zero V
            return zero
        }
        return this.vals[i - 1]
    }

    func Has(k K) bool {
        return this.index[k] > 0
    }

    func Remove(k K) {
        var i = this.index[k]
        if i == 0 {
            return
        }
        delete(this.index, k)
        var n = len(this.keys)
        for j in i..n {
            this.keys[j - 1] = this.keys[j]
            this.vals[j - 1] = this.vals[j]
            this.index[this.keys[j - 1]] = j
        }
        var zk K
        var zv V
        this.keys[n - 1] = zk
        this.vals[n - 1] = zv
        this.keys = this.keys[0..n - 1]
        this.vals = this.vals[0..n - 1]
    }

    func Len() int {
        return len(this.keys)
    }

    func Clear() {
        this.index = nil
        this.keys = nil
        this.vals = nil
    }

    func Keys() []K {
        var out []K = []
        for k in this.keys {
            out = append(out, k)
        }
        return out
    }

    func Values() []V {
        var out []V = []
        for v in this.vals {
            out = append(out, v)
        }
        return out
    }
}

struct Set<T: Comparable> {
    index map[T]int
    items []T

    static func New() Set<T> {
        return Set<T>{}
    }

    func Add(x T) {
        if this.index[x] > 0 {
            return
        }
        if this.index == nil {
            this.index = map[T]int{}
        }
        this.items = append(this.items, x)
        this.index[x] = len(this.items)
    }

    func Has(x T) bool {
        return this.index[x] > 0
    }

    func Remove(x T) {
        var i = this.index[x]
        if i == 0 {
            return
        }
        delete(this.index, x)
        var n = len(this.items)
        for j in i..n {
            this.items[j - 1] = this.items[j]
            this.index[this.items[j - 1]] = j
        }
        var zero T
        this.items[n - 1] = zero
        this.items = this.items[0..n - 1]
    }

    func Len() int {
        return len(this.items)
    }

    func Clear() {
        this.index = nil
        this.items = nil
    }

    func Items() []T {
        var out []T = []
        for x in this.items {
            out = append(out, x)
        }
        return out
    }
}

struct Queue<T> {
    items []T

    static func New() Queue<T> {
        return Queue<T>{}
    }

    func Push(x T) {
        this.items = append(this.items, x)
    }

    func Pop() T! {
        if len(this.items) == 0 {
            return error("queue is empty")
        }
        var x = this.items[0]
        var zero T
        this.items[0] = zero
        this.items = this.items[1..]
        return x
    }

    func Peek() T! {
        if len(this.items) == 0 {
            return error("queue is empty")
        }
        return this.items[0]
    }

    func Len() int {
        return len(this.items)
    }
}

struct Stack<T> {
    items []T

    static func New() Stack<T> {
        return Stack<T>{}
    }

    func Push(x T) {
        this.items = append(this.items, x)
    }

    func Pop() T! {
        var n = len(this.items)
        if n == 0 {
            return error("stack is empty")
        }
        var x = this.items[n - 1]
        var zero T
        this.items[n - 1] = zero
        this.items = this.items[0..n - 1]
        return x
    }

    func Peek() T! {
        if len(this.items) == 0 {
            return error("stack is empty")
        }
        return this.items[len(this.items) - 1]
    }

    func Len() int {
        return len(this.items)
    }
}
"#;

/// Methods that need `==` on the element type: (struct, method).
const NEEDS_EQUATABLE: &[(&str, &str)] = &[("List", "Contains"), ("List", "IndexOf")];

/// The parsed [`SOURCE`] (parsed once per process).
fn collections_ast() -> &'static ast::File {
    static AST: OnceLock<ast::File> = OnceLock::new();
    AST.get_or_init(|| {
        let mut diags = crate::diagnostics::Diagnostics::new();
        let tokens = crate::lexer::lex(SOURCE, &mut diags);
        let file = crate::parser::parse(tokens, &mut diags);
        match file {
            Some(f) if !diags.has_errors() => f,
            _ => panic!("internal error: std.collections source does not parse: {:?}", diags.items),
        }
    })
}

impl<'a> Sema<'a> {
    /// Registers the `std.collections` templates.
    pub(crate) fn register_collections(&mut self) {
        if self.files.is_empty() {
            return;
        }
        let file: &'static ast::File = collections_ast();
        let saved = (self.cur_file, self.cur_ns);
        let ns = self.ensure_ns("std.collections", Span::DUMMY);
        self.cur_ns = ns;
        self.cur_file = 0;
        for d in &file.decls {
            if let ast::Decl::Struct(s) = d {
                let t = self.new_template(&s.name.name, TmplDecl::Struct(s), &s.generics, Vec::new(), false);
                self.nss[ns].members.insert(s.name.name.clone(), (Member::Generic(t), Span::DUMMY, 0));
            }
        }
        (self.cur_file, self.cur_ns) = saved;
    }

    /// Name and type arguments of a `std.collections` instance.
    fn std_collection(&self, sid: u32) -> Option<(&str, &[TypeId])> {
        let (t, args) = self.gen.inst_structs.get(&sid)?;
        let tm = &self.gen.templates[*t as usize];
        (tm.is_std && self.nss[tm.ns].path == "std.collections").then_some((tm.name.as_str(), args.as_slice()))
    }

    /// Called at the end of a struct instantiation: stdlib methods whose
    /// requirements the element type does not meet are not checked.
    pub(crate) fn std_instance_hook(&mut self, sid: u32) {
        let Some((name, args)) = self.std_collection(sid) else { return };
        let elem = args[0];
        if self.is_comparable(elem) {
            return;
        }
        let skip: Vec<u32> = self.structs[sid as usize]
            .methods
            .iter()
            .filter(|(m, _)| NEEDS_EQUATABLE.iter().any(|(s, n)| *s == name && n == m))
            .map(|(_, f)| f.0)
            .collect();
        self.gen.skip_body.extend(skip);
    }

    /// Checks the extra requirements of a stdlib collection method call.
    pub(crate) fn check_std_method(&mut self, sid: u32, method: &str, span: Span) {
        let Some((name, args)) = self.std_collection(sid) else { return };
        if !NEEDS_EQUATABLE.iter().any(|(s, n)| *s == name && *n == method) {
            return;
        }
        let elem = args[0];
        if let Err(msg) = self.satisfies(elem, generics::Bound::Comparable) {
            let sname = self.structs[sid as usize].name.clone();
            self.err(format!("{msg} (required by {sname}.{method})"), span);
        }
    }

    /// Lowercase members of stdlib collections are private to the stdlib.
    pub(crate) fn check_std_private(&mut self, sid: u32, member: &str, span: Span) {
        if is_exported(member) || self.nss[self.cur_ns].is_std {
            return;
        }
        let Some((t, _)) = self.gen.inst_structs.get(&sid) else { return };
        let tm = &self.gen.templates[*t as usize];
        if !tm.is_std {
            return;
        }
        let path = self.nss[tm.ns].path.clone();
        self.err(format!("{member} is private to namespace {path} (only capitalised names are exported)"), span);
    }

    /// `for ... in c` over a collection: iterates the backing slices.
    /// Returns the statement, or gives `seq` back if it is no collection.
    pub(crate) fn collection_for(&mut self, vars: &[ast::Ident], seq: hir::Expr, body: &ast::Block) -> Result<StmtKind, hir::Expr> {
        let Some(sid) = self.struct_of(seq.ty) else { return Err(seq) };
        let Some((name, args)) = self.std_collection(sid) else { return Err(seq) };
        let (name, args) = (name.to_string(), args.to_vec());
        let field = |s: &Self, f: &str| s.structs[sid as usize].fields.iter().position(|x| x.name == f).unwrap() as u32;
        let span = seq.span;
        // Evaluate the collection once into a temp.
        let seq = self.auto_deref(seq);
        let st = seq.ty;
        let tmp = self.temp(st, span);
        let slice_of = |s: &mut Self, f: &str, elem: TypeId| {
            let idx = field(s, f);
            let ty = s.tt.slice(elem);
            mk(ExprKind::Field { base: Box::new(mk(ExprKind::Local(tmp), st, span)), index: idx }, ty, span)
        };
        let label;
        let kind;
        self.push_scope();
        if name == "Map" {
            let (k, v) = (args[0], args[1]);
            if vars.len() > 2 || vars.is_empty() {
                self.err("for ... in over a Map takes one or two variables (key, value)", span);
            }
            let idx = self.temp(self.tt.int, span);
            let key = self.for_var(vars.first(), k);
            let val = self.for_var(vars.get(1), v);
            label = self.push_label(LabelKind::Loop);
            let mut hbody = self.block(body);
            if let Some(vl) = val {
                let vals = slice_of(self, "vals", v);
                let int = self.tt.int;
                let elem = mk(ExprKind::Index { base: Box::new(vals), index: Box::new(mk(ExprKind::Local(idx), int, span)) }, v, span);
                hbody.stmts.insert(0, Stmt { kind: StmtKind::Let { local: vl, init: Some(elem) }, span });
            }
            let keys = slice_of(self, "keys", k);
            kind = StmtKind::ForSlice { label, index: Some(idx), elem: key, seq: keys, body: hbody };
        } else {
            let elem_ty = args[0];
            if vars.len() > 2 || vars.is_empty() {
                self.err("for ... in takes one or two variables", span);
            }
            let (index, elem) = if vars.len() == 1 {
                (None, self.for_var(vars.first(), elem_ty))
            } else {
                let i = self.for_var(vars.first(), self.tt.int);
                (i, self.for_var(vars.get(1), elem_ty))
            };
            label = self.push_label(LabelKind::Loop);
            let hbody = self.block(body);
            let items = slice_of(self, "items", elem_ty);
            kind = StmtKind::ForSlice { label, index, elem, seq: items, body: hbody };
        }
        self.pop_label();
        self.pop_scope();
        let init = self.coerce(seq, st, Ctx::Other);
        let stmts = vec![Stmt { kind: StmtKind::Let { local: tmp, init: Some(init) }, span }, Stmt { kind, span }];
        Ok(StmtKind::Block(hir::Block { stmts, span }))
    }

    fn for_var(&mut self, v: Option<&ast::Ident>, ty: TypeId) -> Option<LocalId> {
        let v = v?;
        if v.name == "_" {
            return None;
        }
        Some(self.declare_local(&v.name, ty, LocalKind::LoopVar, v.span))
    }
}
