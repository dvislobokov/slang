//! Abstract syntax tree for Slang. See DESIGN.md for the language.
//!
//! The parser produces exactly these types; later phases (name resolution,
//! type checking, codegen) consume them. Every node carries a `Span`.

use crate::span::Span;

#[derive(Clone, Debug, PartialEq)]
pub struct Ident {
    pub name: String,
    pub span: Span,
}

/// Dotted path like `std.net.http` or `Point`.
#[derive(Clone, Debug, PartialEq)]
pub struct Path {
    pub segments: Vec<Ident>,
    pub span: Span,
}

impl Path {
    pub fn to_string(&self) -> String {
        self.segments.iter().map(|s| s.name.as_str()).collect::<Vec<_>>().join(".")
    }
}

// ---------------------------------------------------------------------------
// File level
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
pub struct File {
    /// Attributes written before `namespace`.
    pub attrs: Vec<Attribute>,
    pub namespace: Path,
    pub imports: Vec<Import>,
    pub decls: Vec<Decl>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Import {
    pub path: Path,
    pub alias: Option<Ident>,
    pub span: Span,
}

/// `@json(name: "id")`, `@reflect`, `@deprecated("use Save")`.
#[derive(Clone, Debug, PartialEq)]
pub struct Attribute {
    pub path: Path,
    pub args: Vec<AttrArg>,
    pub span: Span,
}

/// `name: value` or a positional `value` inside an attribute.
#[derive(Clone, Debug, PartialEq)]
pub struct AttrArg {
    pub name: Option<Ident>,
    pub value: Expr,
    pub span: Span,
}

/// Generic parameter `T` or `T: Bound` in `func F<T>(..)` / `struct S<T>`.
#[derive(Clone, Debug, PartialEq)]
pub struct GenericParam {
    pub name: Ident,
    pub bound: Option<TypeExpr>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Decl {
    Struct(StructDecl),
    Interface(InterfaceDecl),
    Enum(EnumDecl),
    Func(FuncDecl),
    Var(VarDecl),
    Const(ConstDecl),
}

impl Decl {
    pub fn span(&self) -> Span {
        match self {
            Decl::Struct(d) => d.span,
            Decl::Interface(d) => d.span,
            Decl::Enum(d) => d.span,
            Decl::Func(d) => d.span,
            Decl::Var(d) => d.span,
            Decl::Const(d) => d.span,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct StructDecl {
    pub attrs: Vec<Attribute>,
    pub name: Ident,
    pub generics: Vec<GenericParam>,
    pub fields: Vec<Field>,
    /// Embedded types: a bare type name on its own line inside the struct.
    pub embeds: Vec<TypeExpr>,
    pub methods: Vec<FuncDecl>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Field {
    pub attrs: Vec<Attribute>,
    pub name: Ident,
    pub ty: TypeExpr,
    /// Default value, `message string = "..."` (attribute structs).
    pub default: Option<Expr>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct InterfaceDecl {
    pub attrs: Vec<Attribute>,
    pub name: Ident,
    pub generics: Vec<GenericParam>,
    /// Method signatures; `body` is always `None`.
    pub methods: Vec<FuncDecl>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct EnumDecl {
    pub attrs: Vec<Attribute>,
    pub name: Ident,
    pub variants: Vec<Variant>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Variant {
    pub attrs: Vec<Attribute>,
    pub name: Ident,
    /// Empty for plain variants like `Red`; named fields for `Ok(value int)`.
    pub fields: Vec<Field>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct FuncDecl {
    pub attrs: Vec<Attribute>,
    pub name: Ident,
    pub generics: Vec<GenericParam>,
    pub params: Vec<Param>,
    /// `None` means `void`.
    pub ret: Option<TypeExpr>,
    /// `None` for interface method signatures.
    pub body: Option<Block>,
    pub is_async: bool,
    pub is_static: bool,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Param {
    pub attrs: Vec<Attribute>,
    pub name: Ident,
    pub ty: TypeExpr,
    pub span: Span,
}

/// `var x = 1`, `var x int`, `var (a, b) = f()`.
#[derive(Clone, Debug, PartialEq)]
pub struct VarDecl {
    pub attrs: Vec<Attribute>,
    /// One name normally; several for tuple destructuring `var (a, b) = ...`.
    /// `_` is represented as an Ident named "_".
    pub names: Vec<Ident>,
    pub ty: Option<TypeExpr>,
    pub init: Option<Expr>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ConstDecl {
    pub attrs: Vec<Attribute>,
    pub name: Ident,
    pub ty: Option<TypeExpr>,
    pub value: Expr,
    pub span: Span,
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
pub struct TypeExpr {
    pub kind: TypeKind,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub enum TypeKind {
    /// `int`, `Point`, `app.models.User`, `List<int>`.
    Named { path: Path, generics: Vec<TypeExpr> },
    /// `T!`
    Fallible(Box<TypeExpr>),
    /// `*T`
    Pointer(Box<TypeExpr>),
    /// `weak *T` (the box holds the pointee `T`)
    Weak(Box<TypeExpr>),
    /// `[]T`
    Slice(Box<TypeExpr>),
    /// `[N]T`
    Array(Box<TypeExpr>, u64),
    /// `map[K]V`
    Map(Box<TypeExpr>, Box<TypeExpr>),
    /// `func(int, string) bool`
    Func { params: Vec<TypeExpr>, ret: Option<Box<TypeExpr>> },
    /// `(int, string)`
    Tuple(Vec<TypeExpr>),
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
pub struct Block {
    pub stmts: Vec<Stmt>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Stmt {
    pub kind: StmtKind,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub enum StmtKind {
    Var(VarDecl),
    Const(ConstDecl),
    If(IfStmt),
    For(ForStmt),
    Switch(SwitchStmt),
    Return(Vec<Expr>),
    Break,
    Continue,
    Defer(Expr),
    Expr(Expr),
    Assign { target: Expr, op: AssignOp, value: Expr },
    Block(Block),
}

#[derive(Clone, Debug, PartialEq)]
pub struct IfStmt {
    pub cond: Expr,
    pub then: Block,
    pub else_: Option<Box<ElseBranch>>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ElseBranch {
    If(IfStmt),
    Block(Block),
}

#[derive(Clone, Debug, PartialEq)]
pub struct ForStmt {
    pub kind: ForKind,
    pub body: Block,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ForKind {
    /// `for { }`
    Infinite,
    /// `for cond { }`
    While(Expr),
    /// `for x in iter { }` or `for i, x in iter { }`; `iter` may be a `Range`.
    In { vars: Vec<Ident>, iter: Expr },
}

#[derive(Clone, Debug, PartialEq)]
pub struct SwitchStmt {
    /// `None` for `switch { case cond: ... }`.
    pub subject: Option<Expr>,
    pub cases: Vec<Case>,
    pub default: Option<Vec<Stmt>>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Case {
    pub patterns: Vec<Pattern>,
    pub body: Vec<Stmt>,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Pattern {
    pub kind: PatternKind,
    pub span: Span,
}

/// Syntactic patterns. Disambiguation (value vs guard vs enum variant) is
/// done by the semantic phase, which knows the subject's type.
#[derive(Clone, Debug, PartialEq)]
pub enum PatternKind {
    /// `1`, `"a"`, `n > 100`, `Result.Ok(v)` (parsed as a call expression).
    Expr(Expr),
    /// `3..10`, `..0`, `100..`; a missing end is `None`.
    Range(Option<Expr>, Option<Expr>),
    /// `Circle c`: type followed by a binding name.
    TypeBind { ty: TypeExpr, name: Ident },
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ExprKind {
    Int(u64),
    Float(f64),
    Str(String),
    Char(char),
    Bool(bool),
    Nil,
    This,
    Ident(Ident),
    /// `base.name`
    Field { base: Box<Expr>, name: Ident },
    Call { callee: Box<Expr>, args: Vec<Expr> },
    Index { base: Box<Expr>, index: Box<Expr> },
    Unary { op: UnaryOp, expr: Box<Expr> },
    Binary { op: BinaryOp, left: Box<Expr>, right: Box<Expr> },
    /// `expr?` or `expr?("context")`
    Propagate { expr: Box<Expr>, context: Option<Box<Expr>> },
    /// `try expr catch err { }` / `try expr else value`
    Try { expr: Box<Expr>, handler: TryHandler },
    Await(Box<Expr>),
    /// `Point{X: 1, Y: 2}`; `path` is empty-segment-free, e.g. `Point` or `app.models.User`.
    /// `generics` holds explicit type arguments: `List<int>{}`.
    StructLit { path: Path, generics: Vec<TypeExpr>, fields: Vec<FieldInit> },
    /// Explicit type arguments on a callee: `reflect.TypeOf<User>`, usually
    /// the callee of a `Call`.
    TypeArgs { base: Box<Expr>, args: Vec<TypeExpr> },
    /// `new Point{...}` / `new int`
    New(Box<Expr>),
    /// `[1, 2, 3]`
    SliceLit(Vec<Expr>),
    /// `map[string]int{"a": 1}`
    MapLit { key_ty: Box<TypeExpr>, val_ty: Box<TypeExpr>, entries: Vec<(Expr, Expr)> },
    /// `(a, b)` with two or more elements.
    Tuple(Vec<Expr>),
    /// `a..b`, `a..`, `..b`; only valid as a `for` iterator, an index
    /// (sub-slice `s[1..3]`) or a `case` pattern. A missing end is `None`.
    Range(Option<Box<Expr>>, Option<Box<Expr>>),
    /// `func(x int) int { ... }` or `(x int) => x * 2`
    Lambda { params: Vec<Param>, ret: Option<Box<TypeExpr>>, body: Box<LambdaBody> },
    /// `(expr)` kept for faithful formatting; semantics are those of `expr`.
    Paren(Box<Expr>),
}

#[derive(Clone, Debug, PartialEq)]
pub enum LambdaBody {
    Block(Block),
    Expr(Expr),
}

#[derive(Clone, Debug, PartialEq)]
pub struct FieldInit {
    pub name: Ident,
    pub value: Expr,
    pub span: Span,
}

#[derive(Clone, Debug, PartialEq)]
pub enum TryHandler {
    /// `catch err { ... }`; `name` may be `_`.
    Catch { name: Ident, body: Block },
    /// `else expr`, `else continue`, `else break`, `else return [expr]`.
    Else(Box<ElseValue>),
}

#[derive(Clone, Debug, PartialEq)]
pub enum ElseValue {
    Expr(Expr),
    Continue(Span),
    Break(Span),
    Return(Vec<Expr>, Span),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnaryOp {
    Neg,    // -x
    Not,    // !x
    AddrOf, // &x
    Deref,  // *x
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BinaryOp {
    Add, Sub, Mul, Div, Rem,
    BitAnd, BitOr, BitXor, Shl, Shr,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
}

impl BinaryOp {
    /// Higher binds tighter. Same table as Go.
    pub fn precedence(self) -> u8 {
        use BinaryOp::*;
        match self {
            Mul | Div | Rem | Shl | Shr | BitAnd => 5,
            Add | Sub | BitOr | BitXor => 4,
            Eq | Ne | Lt | Le | Gt | Ge => 3,
            And => 2,
            Or => 1,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AssignOp {
    Assign, Add, Sub, Mul, Div, Rem, BitAnd, BitOr, BitXor, Shl, Shr,
}
