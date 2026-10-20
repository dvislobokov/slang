//! Ownership facts shared by the checker (`sema::ownership`) and code
//! generation: destructors, the position of every expression (does its
//! consumer store the value or only read through it?), and the per-parameter
//! escape analysis.
//!
//! # Rules (see DESIGN.md "Память")
//!
//! * A `*T` value always points at the payload of a whole refcounted heap
//!   object (`new`, `&x` of a variable, which then lives in a heap cell,
//!   `&T{...}`) and owns one reference to it.
//! * An *interior pointer* (`&s.f`, `&a[i]`, `&slice[i]`, `&global`) has no
//!   header of its own. It may only be read through (dereferenced, compared,
//!   field access) or passed to a parameter that does not escape. The caller
//!   keeps the owner alive for the call.
//! * A parameter *escapes* when the callee stores it anywhere (variable,
//!   field, element, closure capture, interface, `weak`), returns it,
//!   reassigns it, takes its address, passes it to an escaping parameter or
//!   to an unknown callee, or when the function is `async` or a lambda.
//! * A parameter is *owned* (retained on entry, released at exit) when the
//!   function is `async` or reassigns it; a parameter of a type with a
//!   destructor is *moved* in. All other parameters are *borrowed*: the
//!   caller guarantees the value stays alive during the call.

use super::*;
use crate::types::Type;

/// How the consumer of an expression uses its value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pos {
    /// Copied, stored or consumed: needs an owning value.
    Store,
    /// Only read through (dereferenced, compared, printed, field or index
    /// base, runtime-call argument).
    Read,
    /// Argument `i` of a direct call of a function.
    Arg(FuncId, usize),
    /// Operand of `&`: the place itself, not its value.
    AddrOf,
    /// Assignment target.
    Target,
}

/// The method `Drop` of struct `sid` (non-static), if declared.
pub fn drop_method(p: &Program, sid: u32) -> Option<FuncId> {
    let s = &p.structs[sid as usize];
    s.methods.iter().find(|(n, f)| n == "Drop" && matches!(p.funcs[f.0 as usize].kind, FuncKind::Method { is_static: false, .. })).map(|(_, f)| *f)
}

/// True if a value of type `t` holds a struct with a destructor by value
/// (directly, in a field, array element, `T!` or enum payload).
pub fn has_dtor(p: &Program, t: TypeId) -> bool {
    match p.types.get(t) {
        Type::Struct(d) => drop_method(p, d.0).is_some() || p.structs[d.0 as usize].fields.iter().any(|f| has_dtor(p, f.ty)),
        Type::Array(e, n) => *n > 0 && has_dtor(p, *e),
        Type::Tuple(ts) => ts.iter().any(|t| has_dtor(p, *t)),
        Type::Fallible(t) => has_dtor(p, *t),
        Type::Enum(d) => p.enums[d.0 as usize].variants.iter().any(|v| v.fields.iter().any(|f| has_dtor(p, f.1))),
        _ => false,
    }
}

/// True for an `&x` whose result is an interior pointer (no header).
pub fn is_interior_addr(e: &Expr) -> bool {
    let ExprKind::AddrOf(inner) = &e.kind else { return false };
    match &inner.kind {
        ExprKind::Field { .. } | ExprKind::Index { .. } | ExprKind::Global(_) => true,
        ExprKind::Deref(y) => is_interior_addr(y),
        _ => false,
    }
}

/// Calls `f(expr, position)` for every expression of `func`'s body, parents
/// before children.
pub fn walk_function(p: &Program, func: &Function, f: &mut dyn FnMut(&Expr, Pos)) {
    let mut w = Walker { p, f };
    w.block(&func.body);
}

/// Calls `f` for `e` (in position `pos`) and all its subexpressions.
pub fn walk_expr(p: &Program, e: &Expr, pos: Pos, f: &mut dyn FnMut(&Expr, Pos)) {
    let mut w = Walker { p, f };
    w.expr(e, pos);
}

struct Walker<'p, 'f> {
    p: &'p Program,
    f: &'f mut dyn FnMut(&Expr, Pos),
}

impl Walker<'_, '_> {
    fn block(&mut self, b: &Block) {
        for s in &b.stmts {
            self.stmt(s);
        }
    }

    fn rv(&mut self, rv: &ReturnValue) {
        match rv {
            ReturnValue::Void => {}
            ReturnValue::Value(e) | ReturnValue::Error(e) => self.expr(e, Pos::Store),
        }
    }

    fn stmt(&mut self, s: &Stmt) {
        use Pos::*;
        match &s.kind {
            StmtKind::Let { init, .. } => {
                if let Some(e) = init {
                    self.expr(e, Store)
                }
            }
            StmtKind::LetTuple { init, .. } => self.expr(init, Store),
            StmtKind::Assign { target, value } => {
                self.expr(target, Target);
                self.expr(value, Store);
            }
            StmtKind::CompoundAssign { target, value, .. } => {
                self.expr(target, Target);
                self.expr(value, Read);
            }
            StmtKind::Expr(e) | StmtKind::Discard(e) => self.expr(e, Read),
            StmtKind::If { cond, then, else_ } => {
                self.expr(cond, Read);
                self.block(then);
                if let Some(e) = else_ {
                    self.block(e);
                }
            }
            StmtKind::Loop { cond, body, .. } => {
                if let Some(c) = cond {
                    self.expr(c, Read);
                }
                self.block(body);
            }
            StmtKind::ForRange { start, end, body, .. } => {
                self.expr(start, Read);
                self.expr(end, Read);
                self.block(body);
            }
            StmtKind::ForSlice { seq, body, .. } => {
                self.expr(seq, Store);
                self.block(body);
            }
            StmtKind::ForString { string, body, .. } => {
                self.expr(string, Read);
                self.block(body);
            }
            StmtKind::ForMap { map, body, .. } => {
                self.expr(map, Read);
                self.block(body);
            }
            StmtKind::Switch(sw) => {
                if let Some((_, e)) = &sw.subject {
                    self.expr(e, Store);
                }
                for c in &sw.cases {
                    for p in &c.patterns {
                        if let Pattern::Cond(e) = p {
                            self.expr(e, Read);
                        }
                    }
                    self.block(&c.body);
                }
                if let Some(d) = &sw.default {
                    self.block(d);
                }
            }
            StmtKind::Return(rv) => self.rv(rv),
            StmtKind::Break(_) | StmtKind::Continue(_) => {}
            StmtKind::Defer(e) => self.expr(e, Store),
            StmtKind::Block(b) => self.block(b),
            StmtKind::TryBlock { body, handler, .. } => {
                self.block(body);
                self.block(handler);
            }
        }
    }

    fn expr(&mut self, e: &Expr, pos: Pos) {
        use Pos::*;
        (self.f)(e, pos);
        match &e.kind {
            ExprKind::Const(_) | ExprKind::Nil | ExprKind::Zero | ExprKind::Local(_) | ExprKind::Upvalue(_) | ExprKind::Global(_) | ExprKind::FuncRef(_) | ExprKind::Closure { .. } => {}
            ExprKind::Field { base, .. } => self.expr(base, Read),
            ExprKind::Deref(x) => self.expr(x, Read),
            ExprKind::AddrOf(x) => match &x.kind {
                // `&*p` is `p` itself.
                ExprKind::Deref(y) => self.expr(y, pos),
                _ => self.expr(x, AddrOf),
            },
            ExprKind::New(x) => self.expr(x, Store),
            ExprKind::Index { base, index } => {
                self.expr(base, Read);
                self.expr(index, Read);
            }
            ExprKind::MapGet { map, key } => {
                self.expr(map, Read);
                self.expr(key, Read);
            }
            ExprKind::SubSlice { base, lo, hi } => {
                // An array is sub-sliced in place: the slice points into it.
                let arr = matches!(self.p.types.get(base.ty), Type::Array(..));
                self.expr(base, if arr { AddrOf } else { Read });
                for x in [lo, hi].into_iter().flatten() {
                    self.expr(x, Read);
                }
            }
            ExprKind::Unary { operand, .. } => self.expr(operand, Read),
            ExprKind::Binary { lhs, rhs, .. } => {
                self.expr(lhs, Read);
                self.expr(rhs, Read);
            }
            ExprKind::IsNil { value, .. } => self.expr(value, Read),
            ExprKind::Convert { kind, value } => self.expr(value, if *kind == ConvKind::PtrToWeak { Store } else { Read }),
            ExprKind::Call { callee, args } => match callee {
                Callee::Func(fid) => {
                    for (i, a) in args.iter().enumerate() {
                        self.expr(a, Arg(*fid, i));
                    }
                }
                Callee::Extern(_) => {
                    for a in args {
                        self.expr(a, Read);
                    }
                }
                Callee::Interface { recv, .. } => {
                    self.expr(recv, Read);
                    for a in args {
                        self.expr(a, Store);
                    }
                }
                Callee::Value(c) => {
                    self.expr(c, Read);
                    for a in args {
                        self.expr(a, Store);
                    }
                }
            },
            ExprKind::StructLit { fields, .. } => {
                for (_, x) in fields {
                    self.expr(x, Store);
                }
            }
            ExprKind::SliceLit(xs) | ExprKind::Tuple(xs) | ExprKind::EnumLit { args: xs, .. } => {
                for x in xs {
                    self.expr(x, Store);
                }
            }
            ExprKind::MapLit(kvs) => {
                for (k, v) in kvs {
                    self.expr(k, Store);
                    self.expr(v, Store);
                }
            }
            ExprKind::ToInterface { value } => self.expr(value, Store),
            ExprKind::Propagate { value, context } => {
                self.expr(value, Store);
                if let Some(c) = context {
                    self.expr(c, Read);
                }
            }
            ExprKind::TryCatch { value, handler, .. } => {
                self.expr(value, Store);
                self.block(handler);
            }
            ExprKind::TryElse { value, fallback } => {
                self.expr(value, Store);
                match &**fallback {
                    TryFallback::Value(x) => self.expr(x, Store),
                    TryFallback::Return(rv) => self.rv(rv),
                    TryFallback::Break(_) | TryFallback::Continue(_) => {}
                }
            }
            ExprKind::Await(t) => self.expr(t, Read),
            ExprKind::TaskAll(ts) => {
                for t in ts {
                    self.expr(t, Read);
                }
            }
            ExprKind::Builtin { op, args } => {
                for (i, a) in args.iter().enumerate() {
                    let p = match op {
                        Builtin::Append => Store,
                        Builtin::LenArray if i == 0 => AddrOf,
                        _ => Read,
                    };
                    self.expr(a, p);
                }
            }
            ExprKind::ToString(v) => self.expr(v, Read),
            ExprKind::Print(items) => {
                for it in items {
                    if let Some(a) = &it.arg {
                        self.expr(a, Read);
                    }
                }
            }
        }
    }
}

/// Per-parameter ownership facts of every function.
#[derive(Debug, Clone, Default)]
pub struct ParamFacts {
    /// `escapes[f][k]`: parameter `k` of function `f` escapes.
    pub escapes: Vec<Vec<bool>>,
    /// `owned[f][k]`: the callee retains parameter `k` on entry.
    pub owned: Vec<Vec<bool>>,
    /// `moved[f][k]`: parameter `k` has a destructor and is moved in.
    pub moved: Vec<Vec<bool>>,
}

impl ParamFacts {
    pub fn escapes(&self, f: FuncId, k: usize) -> bool {
        self.escapes.get(f.0 as usize).and_then(|v| v.get(k)).copied().unwrap_or(true)
    }
    pub fn owned(&self, f: FuncId, k: usize) -> bool {
        self.owned.get(f.0 as usize).and_then(|v| v.get(k)).copied().unwrap_or(true)
    }
    pub fn moved(&self, f: FuncId, k: usize) -> bool {
        self.moved.get(f.0 as usize).and_then(|v| v.get(k)).copied().unwrap_or(false)
    }
}

/// Computes [`ParamFacts`] for the whole program (greatest fixpoint: a
/// parameter only passed on to non-escaping parameters does not escape,
/// also through recursion).
pub fn param_facts(p: &Program) -> ParamFacts {
    let n = p.funcs.len();
    let mut facts = ParamFacts { escapes: Vec::with_capacity(n), owned: Vec::with_capacity(n), moved: Vec::with_capacity(n) };
    // Local index -> parameter index, per function.
    let mut param_of: Vec<Vec<Option<usize>>> = Vec::with_capacity(n);
    for f in &p.funcs {
        let mut m = vec![None; f.locals.len()];
        for (k, l) in f.params.iter().enumerate() {
            m[l.0 as usize] = Some(k);
        }
        param_of.push(m);
        let base = f.is_async || matches!(f.kind, FuncKind::Lambda { .. });
        let esc: Vec<bool> = f.params.iter().map(|l| {
            let loc = &f.locals[l.0 as usize];
            base || loc.captured || loc.address_taken
        }).collect();
        facts.escapes.push(esc);
        facts.moved.push(f.params.iter().map(|l| has_dtor(p, f.locals[l.0 as usize].ty)).collect());
    }
    // Reassigned parameters (owned) and local escapes.
    for (fi, f) in p.funcs.iter().enumerate() {
        let mut owned: Vec<bool> = f.params.iter().map(|_| f.is_async).collect();
        let po = &param_of[fi];
        walk_function(p, f, &mut |e, pos| {
            if let ExprKind::Local(l) = &e.kind {
                if let Some(k) = po[l.0 as usize] {
                    if pos == Pos::Target {
                        owned[k] = true;
                    }
                }
            }
        });
        for (k, o) in owned.iter().enumerate() {
            if *o {
                facts.escapes[fi][k] = true;
            }
        }
        facts.owned.push(owned);
    }
    loop {
        let mut changed = false;
        for (fi, f) in p.funcs.iter().enumerate() {
            let po = &param_of[fi];
            let mut marks = Vec::new();
            {
                let esc = &facts.escapes;
                walk_function(p, f, &mut |e, pos| {
                    if let ExprKind::Local(l) = &e.kind {
                        if let Some(k) = po[l.0 as usize] {
                            let escapes = match pos {
                                Pos::Store | Pos::Target => true,
                                Pos::Arg(g, i) => esc.get(g.0 as usize).and_then(|v| v.get(i)).copied().unwrap_or(true),
                                Pos::Read | Pos::AddrOf => false,
                            };
                            if escapes {
                                marks.push(k);
                            }
                        }
                    }
                });
            }
            for k in marks {
                if !facts.escapes[fi][k] {
                    facts.escapes[fi][k] = true;
                    changed = true;
                }
            }
        }
        if !changed {
            break;
        }
    }
    facts
}
