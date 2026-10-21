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

// List is a growable list of T backed by a slice.
struct List<T> {
    items []T

    // New returns an empty list (the zero value works too).
    static func New() List<T> {
        return List<T>{}
    }

    // Add appends x.
    func Add(x T) {
        this.items = append(this.items, x)
    }

    // Len returns the number of elements.
    func Len() int {
        return len(this.items)
    }

    // Get returns element i; it panics if i is out of range.
    func Get(i int) T {
        return this.items[i]
    }

    // Set replaces element i; it panics if i is out of range.
    func Set(i int, x T) {
        this.items[i] = x
    }

    // Insert inserts x before element i (i == Len() appends); it panics if i is out of range.
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

    // Remove deletes element i, shifting the rest down; it panics if i is out of range.
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

    // IndexOf returns the index of the first element equal to x, or -1.
    func IndexOf(x T) int {
        for i, y in this.items {
            if y == x {
                return i
            }
        }
        return -1
    }

    // Contains reports whether an element equals x.
    func Contains(x T) bool {
        return this.IndexOf(x) >= 0
    }

    // Clear removes every element.
    func Clear() {
        this.items = nil
    }

    // Items returns the elements as a new slice.
    func Items() []T {
        var out []T = []
        for x in this.items {
            out = append(out, x)
        }
        return out
    }
}

// Map is a hash map that keeps insertion order (Keys and Values iterate in that order).
struct Map<K: Comparable, V> {
    index map[K]int
    keys []K
    vals []V

    // New returns an empty map (the zero value works too).
    static func New() Map<K, V> {
        return Map<K, V>{}
    }

    // Set inserts or replaces the value of k; a new key goes last.
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

    // Get returns the value of k, or the zero value of V if it is missing.
    func Get(k K) V {
        var i = this.index[k]
        if i == 0 {
            var zero V
            return zero
        }
        return this.vals[i - 1]
    }

    // Has reports whether k is present.
    func Has(k K) bool {
        return this.index[k] > 0
    }

    // Remove deletes k (nothing if it is missing).
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

    // Len returns the number of entries.
    func Len() int {
        return len(this.keys)
    }

    // Clear removes every entry.
    func Clear() {
        this.index = nil
        this.keys = nil
        this.vals = nil
    }

    // Keys returns the keys in insertion order.
    func Keys() []K {
        var out []K = []
        for k in this.keys {
            out = append(out, k)
        }
        return out
    }

    // Values returns the values in insertion order.
    func Values() []V {
        var out []V = []
        for v in this.vals {
            out = append(out, v)
        }
        return out
    }
}

// Set is a set of T that keeps insertion order.
struct Set<T: Comparable> {
    index map[T]int
    items []T

    // New returns an empty set (the zero value works too).
    static func New() Set<T> {
        return Set<T>{}
    }

    // Add inserts x (nothing if it is present).
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

    // Has reports whether x is in the set.
    func Has(x T) bool {
        return this.index[x] > 0
    }

    // Remove deletes x (nothing if it is missing).
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

    // Len returns the number of elements.
    func Len() int {
        return len(this.items)
    }

    // Clear removes every element.
    func Clear() {
        this.index = nil
        this.items = nil
    }

    // Items returns the elements in insertion order, as a new slice.
    func Items() []T {
        var out []T = []
        for x in this.items {
            out = append(out, x)
        }
        return out
    }
}

// Queue is a first-in, first-out queue.
struct Queue<T> {
    items []T

    // New returns an empty queue (the zero value works too).
    static func New() Queue<T> {
        return Queue<T>{}
    }

    // Push adds x at the back.
    func Push(x T) {
        this.items = append(this.items, x)
    }

    // Pop removes and returns the front element; an error if the queue is empty.
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

    // Peek returns the front element without removing it; an error if the queue is empty.
    func Peek() T! {
        if len(this.items) == 0 {
            return error("queue is empty")
        }
        return this.items[0]
    }

    // Len returns the number of elements.
    func Len() int {
        return len(this.items)
    }
}

// Stack is a last-in, first-out stack.
struct Stack<T> {
    items []T

    // New returns an empty stack (the zero value works too).
    static func New() Stack<T> {
        return Stack<T>{}
    }

    // Push puts x on top.
    func Push(x T) {
        this.items = append(this.items, x)
    }

    // Pop removes and returns the top element; an error if the stack is empty.
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

    // Peek returns the top element without removing it; an error if the stack is empty.
    func Peek() T! {
        if len(this.items) == 0 {
            return error("stack is empty")
        }
        return this.items[len(this.items) - 1]
    }

    // Len returns the number of elements.
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
        let text = super::stdfs::load("std.collections", "collections.sl", SOURCE);
        let tokens = crate::lexer::lex(&text, &mut diags);
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
        // Only the List methods need `==` (the newer collections have
        // their own constraints, or no type parameter at all: BitSet).
        let Some(&elem) = args.first() else { return };
        if self.satisfies(elem, generics::Bound::Equatable).is_ok() {
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
        if args.is_empty() || !NEEDS_EQUATABLE.iter().any(|(s, n)| *s == name && *n == method) {
            return;
        }
        let elem = args[0];
        if let Err(msg) = self.satisfies(elem, generics::Bound::Equatable) {
            let sname = self.structs[sid as usize].name.clone();
            self.err(format!("{msg} (required by {sname}.{method})"), span);
        }
    }

    /// Lowercase members of stdlib collections are private to the stdlib.
    pub(crate) fn check_std_private(&mut self, sid: u32, member: &str, span: Span) {
        // Attribute structs (`cli.flag`) are data: their fields are read
        // through `Attribute<A>()` like the builtin `reflect.json` ones.
        if is_exported(member) || self.nss[self.cur_ns].is_std || self.structs[sid as usize].is_attribute {
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
        // The slice-backed core collections only (stdlib_src/collections2.sl
        // types iterate through their Items()/Keys() methods).
        if !matches!(name, "List" | "Map" | "Set" | "Queue" | "Stack") {
            return Err(seq);
        }
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
