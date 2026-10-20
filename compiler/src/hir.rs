//! Typed, fully resolved intermediate representation produced by
//! [`crate::sema::analyze`] and consumed by code generation.
//!
//! # Contract for code generation
//!
//! * Every [`Expr`] has a concrete [`TypeId`] (never `Infer`, `Invalid` or an
//!   untyped constant); every name is resolved to an id: [`LocalId`] (a slot
//!   in the enclosing [`Function::locals`]), upvalue index (closure capture),
//!   [`GlobalId`], [`FuncId`], [`ExternId`], struct field index, enum variant
//!   index, or interface method index.
//! * Struct, interface and enum types appear as `Type::Struct(DeclId)`,
//!   `Type::Interface(DeclId)`, `Type::Enum(DeclId)`; the `DeclId` indexes
//!   [`Program::structs`], [`Program::interfaces`], [`Program::enums`]
//!   respectively. The built-in `error` interface is `Type::Error` (one
//!   method, `Message() string`, index 0) and `any` is `Type::Any`.
//! * Sugar is already lowered: implicit `&` for method receivers, auto-deref
//!   for field access through pointers ([`ExprKind::Deref`] is explicit),
//!   embedded-field promotion (explicit nested [`ExprKind::Field`]s),
//!   untyped constants (always a typed [`ExprKind::Const`]), interface
//!   conversions ([`ExprKind::ToInterface`]), `println`
//!   ([`ExprKind::Print`]), `fmt.Format` (an extern call whose variadic
//!   arguments are a `[]string` of [`ExprKind::ToString`]s), switch value
//!   patterns (boolean [`Pattern::Cond`]s over the subject local),
//!   `defer` (a closure value, see [`StmtKind::Defer`]).
//! * Methods are ordinary [`Function`]s; non-static methods take the
//!   receiver `this` as their first parameter, of type `*S`.
//! * `async func f() T` is a [`Function`] with `is_async = true` and
//!   `ret = T`; calling it yields `Task<T>`, and its `Type::Func` signature
//!   returns `Task<T>`. `return` inside it completes the task.
//! * Spans are byte ranges in the file given by the enclosing item's `file`
//!   field (an index into the files passed to `analyze`).
//!
//! # Evaluation order
//!
//! Left to right everywhere: call arguments, struct/slice/map literal
//! elements, binary operands (with short-circuit for `And`/`Or`).

use crate::span::Span;
use crate::types::{TypeId, TypeTable};

/// Index into [`Program::funcs`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct FuncId(pub u32);
/// Index into [`Program::globals`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct GlobalId(pub u32);
/// Index into [`Program::externs`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ExternId(pub u32);
/// Index into the enclosing [`Function::locals`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct LocalId(pub u32);
/// Index into [`Program::namespaces`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct NamespaceId(pub u32);
/// Identifies a loop or switch inside one function, for `break`/`continue`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Label(pub u32);

/// Index of a source file in the slice given to `sema::analyze`.
pub type FileId = usize;

// ---------------------------------------------------------------------------
// Program and declarations
// ---------------------------------------------------------------------------

/// A whole analysed program.
#[derive(Debug)]
pub struct Program {
    /// All semantic types referenced by the program.
    pub types: TypeTable,
    /// User namespaces (stdlib namespaces are not listed).
    pub namespaces: Vec<Namespace>,
    pub structs: Vec<Struct>,
    pub interfaces: Vec<Interface>,
    pub enums: Vec<Enum>,
    /// Free functions, methods, static methods and lifted lambdas.
    pub funcs: Vec<Function>,
    /// Top-level `var`s and `const`s (file order, then declaration order).
    pub globals: Vec<Global>,
    /// Order in which to run global initialisers at startup: a global whose
    /// initialiser names another global comes after it. (Dependencies
    /// through function calls are not tracked.)
    pub global_init_order: Vec<GlobalId>,
    /// Runtime functions referenced by the program (stdlib externs).
    pub externs: Vec<Extern>,
    /// The entry point: `main` of the root namespace (the namespace with the
    /// shortest path that declares `main`). `None` for libraries. It takes no
    /// parameters and returns `void` (it may be `async`).
    pub entry: Option<FuncId>,
}

/// A user namespace (merged across files).
#[derive(Debug, Clone)]
pub struct Namespace {
    /// Full dotted path, e.g. `app.models`.
    pub path: String,
    /// Attributes written before `namespace` in any of its files.
    pub attrs: Vec<Attr>,
}

/// A struct declaration.
#[derive(Debug, Clone)]
pub struct Struct {
    pub name: String,
    /// `namespace.Name`.
    pub full_name: String,
    pub namespace: NamespaceId,
    /// Fields in declaration order; embedded structs are ordinary fields
    /// (named after the embedded type, `embedded = true`) placed first, in
    /// the order written.
    pub fields: Vec<StructField>,
    /// Methods (static and non-static) by name.
    pub methods: Vec<(String, FuncId)>,
    pub attrs: Vec<Attr>,
    /// True if declared with `@attribute`.
    pub is_attribute: bool,
    pub exported: bool,
    pub file: FileId,
    pub span: Span,
}

/// A struct field.
#[derive(Debug, Clone)]
pub struct StructField {
    pub name: String,
    pub ty: TypeId,
    /// Embedded (promoted) field, e.g. `Animal` inside `Dog`.
    pub embedded: bool,
    /// Exported (capitalised) name.
    pub exported: bool,
    /// Default value (only allowed in `@attribute` structs).
    pub default: Option<ConstValue>,
    pub attrs: Vec<Attr>,
    pub span: Span,
}

/// An interface declaration: an ordered method set. Interface values store
/// a pointer to the dynamic value plus a method table in this order.
#[derive(Debug, Clone)]
pub struct Interface {
    pub name: String,
    pub full_name: String,
    pub namespace: NamespaceId,
    pub methods: Vec<MethodSig>,
    pub attrs: Vec<Attr>,
    pub exported: bool,
    pub file: FileId,
    pub span: Span,
}

/// A method signature in an interface (without the receiver).
#[derive(Debug, Clone)]
pub struct MethodSig {
    pub name: String,
    pub params: Vec<TypeId>,
    pub ret: TypeId,
}

/// An enum. Values are `{ tag: variant index, payload }`; plain enums (no
/// variant has fields) can be represented by the tag alone.
#[derive(Debug, Clone)]
pub struct Enum {
    pub name: String,
    pub full_name: String,
    pub namespace: NamespaceId,
    pub variants: Vec<Variant>,
    /// True if no variant has fields.
    pub is_plain: bool,
    pub attrs: Vec<Attr>,
    pub exported: bool,
    pub file: FileId,
    pub span: Span,
}

/// One enum variant; `fields` is empty for plain variants.
#[derive(Debug, Clone)]
pub struct Variant {
    pub name: String,
    pub fields: Vec<(String, TypeId)>,
    pub attrs: Vec<Attr>,
    pub span: Span,
}

/// What kind of function a [`Function`] is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FuncKind {
    /// Top-level `func`.
    Free,
    /// Method of a struct; `params[0]` is `this: *S` unless `is_static`.
    Method { struct_id: u32, is_static: bool },
    /// Lifted lambda (also used for synthesized `defer` thunks). It is only
    /// reachable through [`ExprKind::Closure`], which supplies its captures.
    Lambda { parent: FuncId },
}

/// A function body ready for codegen.
#[derive(Debug, Clone)]
pub struct Function {
    /// Source name (`Move`, `main`, `lambda#3`).
    pub name: String,
    /// `namespace.Name` or `namespace.Struct.Method`; lambdas get
    /// `parent$lambdaN`.
    pub full_name: String,
    /// Link-level symbol name, unique in the program (`slang_` prefixed,
    /// dots replaced by `__`).
    pub symbol: String,
    pub kind: FuncKind,
    pub namespace: NamespaceId,
    /// Parameter locals in order (receiver first for methods).
    pub params: Vec<LocalId>,
    /// Declared result type, possibly `Fallible`; `void` for no result.
    /// For async functions this is the task's result type `T`, not `Task<T>`.
    pub ret: TypeId,
    pub is_async: bool,
    /// All locals: parameters, `var`s, loop variables, pattern bindings,
    /// catch variables and compiler temporaries.
    pub locals: Vec<Local>,
    /// Capture slots of a lambda; [`ExprKind::Upvalue`] indexes this.
    pub captures: Vec<Capture>,
    pub body: Block,
    pub attrs: Vec<Attr>,
    pub exported: bool,
    pub file: FileId,
    pub span: Span,
}

/// A local variable slot.
#[derive(Debug, Clone)]
pub struct Local {
    pub name: String,
    pub ty: TypeId,
    pub kind: LocalKind,
    /// Captured by a closure: must live in a heap cell. Each execution of the
    /// `Let` (or loop iteration, for loop variables) creates a fresh cell, so
    /// closures created in different iterations see different variables.
    pub captured: bool,
    /// `&x` is taken somewhere (escape analysis input).
    pub address_taken: bool,
    pub span: Span,
}

/// Origin of a [`Local`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LocalKind {
    Param,
    /// The implicit receiver of a method.
    This,
    Var,
    /// Loop variable of a `for ... in`.
    LoopVar,
    /// `case Circle c`, `case Result.Ok(v)` binding.
    PatternBinding,
    /// `catch err` variable (type `error`).
    CatchVar,
    /// Compiler temporary (switch subject, deferred argument, ...).
    Temp,
}

/// A captured variable of a lambda.
#[derive(Debug, Clone)]
pub struct Capture {
    pub name: String,
    pub ty: TypeId,
    /// Where the closure-creating function finds the variable.
    pub source: CaptureSource,
}

/// Location of a captured variable in the function that creates the closure.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CaptureSource {
    /// A local of the creating function (marked `captured`).
    Local(LocalId),
    /// An upvalue of the creating function (itself a lambda).
    Upvalue(u32),
}

/// A top-level `var` or `const`.
#[derive(Debug, Clone)]
pub struct Global {
    pub name: String,
    pub full_name: String,
    pub symbol: String,
    pub namespace: NamespaceId,
    pub ty: TypeId,
    pub is_const: bool,
    /// Compile-time value (for constants that fold, e.g. `const Pi = 3.14`).
    /// Uses of such constants are already inlined as [`ExprKind::Const`].
    pub value: Option<ConstValue>,
    /// Initialiser run at program start (before `main`) when `value` is
    /// `None`; `None` with `value == None` means the zero value.
    pub init: Option<Expr>,
    pub attrs: Vec<Attr>,
    pub exported: bool,
    pub file: FileId,
    pub span: Span,
}

/// A runtime (C) function referenced by the program.
#[derive(Debug, Clone)]
pub struct Extern {
    /// Slang path, e.g. `std.strings.Split`.
    pub path: String,
    /// C symbol, e.g. `slang_strings_split`.
    pub symbol: String,
    pub params: Vec<TypeId>,
    pub ret: TypeId,
}

/// A compile-time constant.
#[derive(Debug, Clone, PartialEq)]
pub enum ConstValue {
    /// Any integer type; the expression's type gives width/signedness.
    Int(i128),
    Float(f64),
    Bool(bool),
    Char(char),
    Str(String),
    /// Plain enum variant: (enum DeclId, variant index).
    Enum(u32, u32),
    /// Slice constant (attribute arguments only).
    List(Vec<ConstValue>),
    /// A symbolic name, used for `@attribute(targets: [...])` entries.
    Name(String),
}

/// An evaluated attribute on a declaration.
#[derive(Debug, Clone)]
pub struct Attr {
    pub kind: AttrKind,
    /// Argument values by parameter name (defaults filled in for user
    /// attributes; only supplied arguments for builtins).
    pub args: Vec<(String, ConstValue)>,
    pub span: Span,
}

/// Which attribute this is.
#[derive(Debug, Clone, PartialEq)]
pub enum AttrKind {
    /// One of `attribute`, `reflect`, `deprecated`, `inline`, `test`, `json`.
    Builtin(String),
    /// A user struct declared with `@attribute` (index into `structs`).
    User(u32),
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

/// A sequence of statements with its own scope.
#[derive(Debug, Clone, Default)]
pub struct Block {
    pub stmts: Vec<Stmt>,
    pub span: Span,
}

/// A statement.
#[derive(Debug, Clone)]
pub struct Stmt {
    pub kind: StmtKind,
    pub span: Span,
}

/// Statement kinds.
#[derive(Debug, Clone)]
pub enum StmtKind {
    /// Declare and initialise a local; `None` means the zero value.
    Let { local: LocalId, init: Option<Expr> },
    /// `var (a, b) = tuple`; `None` slots (`_`) are discarded.
    LetTuple { locals: Vec<Option<LocalId>>, init: Expr },
    /// `target = value`. `target` is a place expression: `Local`,
    /// `Upvalue`, `Global`, `Field` (of a place or of `Deref`), `Deref`,
    /// `Index` (slice element, or array element of a place) or `MapGet`
    /// (which then means "insert or overwrite").
    Assign { target: Expr, value: Expr },
    /// `target op= value` (`op` is never a comparison). For a `MapGet`
    /// target a missing key reads as the zero value and is then inserted.
    CompoundAssign { target: Expr, op: BinOp, value: Expr },
    /// Expression evaluated for its effects (result discarded; never
    /// fallible).
    Expr(Expr),
    /// `_ = e`: evaluate and drop, including a possible error of a `T!`.
    Discard(Expr),
    If { cond: Expr, then: Block, else_: Option<Block> },
    /// `for cond {}` or `for {}` (`cond` is `None`).
    Loop { label: Label, cond: Option<Expr>, body: Block },
    /// `for v in start..end` over integers, half-open; `end` is evaluated
    /// once before the loop. `var` has the integer type of the bounds.
    ForRange { label: Label, var: Option<LocalId>, start: Expr, end: Expr, body: Block },
    /// `for i, x in seq` over a slice or array; `seq` is evaluated once.
    ForSlice { label: Label, index: Option<LocalId>, elem: Option<LocalId>, seq: Expr, body: Block },
    /// `for i, ch in s`: decodes UTF-8 code points; `index` is the byte
    /// offset (int), `ch` the `char`.
    ForString { label: Label, index: Option<LocalId>, ch: Option<LocalId>, string: Expr, body: Block },
    /// `for k, v in m`, unspecified order.
    ForMap { label: Label, key: Option<LocalId>, value: Option<LocalId>, map: Expr, body: Block },
    /// See [`Switch`].
    Switch(Switch),
    Return(ReturnValue),
    /// Leave the loop or switch with this label.
    Break(Label),
    /// Next iteration of the loop with this label.
    Continue(Label),
    /// Push a deferred call. The operand is a closure value of type
    /// `func()` whose captures already hold the evaluated arguments (sema
    /// spills arguments into fresh temporaries before this statement).
    /// Deferred closures run LIFO when the function returns by any path
    /// (`return`, error return via `?`, falling off the end).
    Defer(Expr),
    Block(Block),
}

/// What a `return` returns.
#[derive(Debug, Clone)]
pub enum ReturnValue {
    /// `return` in a `void` or `void!` function (success).
    Void,
    /// A value of the function's result type `T` (for `T!`: success).
    Value(Expr),
    /// In a `T!` function: fail with this error (type `error`).
    Error(Expr),
}

/// A `switch`. Cases are tried in order; the first matching case runs and
/// the switch ends (no fallthrough). `break` inside leaves the switch.
#[derive(Debug, Clone)]
pub struct Switch {
    pub label: Label,
    /// The subject, stored once in a temp local; patterns refer to it.
    pub subject: Option<(LocalId, Expr)>,
    pub cases: Vec<Case>,
    pub default: Option<Block>,
    pub span: Span,
}

/// One `case` with one or more alternative patterns.
#[derive(Debug, Clone)]
pub struct Case {
    pub patterns: Vec<Pattern>,
    pub body: Block,
    pub span: Span,
}

/// A lowered switch pattern.
#[derive(Debug, Clone)]
pub enum Pattern {
    /// Matches when the boolean expression is true. Value, list and range
    /// patterns are lowered to comparisons against the subject local;
    /// guards are used as is.
    Cond(Expr),
    /// The subject (an enum) has this variant; payload fields are bound to
    /// the given locals (`None` = `_`), which are assigned before the body.
    Variant { variant: u32, bindings: Vec<Option<LocalId>> },
    /// The subject (an interface value) holds a value whose dynamic type is
    /// exactly `ty`; the value is copied into `binding`.
    Type { ty: TypeId, binding: Option<LocalId> },
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

/// A typed expression.
#[derive(Debug, Clone)]
pub struct Expr {
    pub kind: ExprKind,
    pub ty: TypeId,
    pub span: Span,
}

/// Expression kinds.
#[derive(Debug, Clone)]
pub enum ExprKind {
    /// Typed constant; `ty` says which integer/float/... type.
    Const(ConstValue),
    /// `nil` of a nilable type (`ty`): pointer, slice, map, func, interface.
    Nil,
    /// Zero value of `ty`.
    Zero,
    Local(LocalId),
    /// Captured variable `i` of the current lambda (by reference).
    Upvalue(u32),
    Global(GlobalId),
    /// A named function used as a value (type `func(...)`).
    FuncRef(FuncId),
    /// Field `index` of a struct value (`base.ty` is a struct type).
    Field { base: Box<Expr>, index: u32 },
    /// `*p` (panics on nil).
    Deref(Box<Expr>),
    /// `&e`. If `e` is not a place, it is first copied into a fresh heap
    /// box (used for method calls on rvalues). `&x` of a variable points at
    /// its heap cell; `&` of a field, element or global is an *interior
    /// pointer*, which sema only allows in read-only positions and as an
    /// argument for a non-escaping parameter (see [`ownership`]).
    AddrOf(Box<Expr>),
    /// `new e`: heap-allocate a copy of `e`, result `*T` (owned).
    New(Box<Expr>),
    /// Element of a slice or array (bounds-checked).
    Index { base: Box<Expr>, index: Box<Expr> },
    /// `m[k]`: value or zero value if missing.
    MapGet { map: Box<Expr>, key: Box<Expr> },
    /// `s[lo..hi]` of a slice, array (place) or string; missing bounds mean
    /// `0` / `len`. Result is a slice (for arrays) or string.
    SubSlice { base: Box<Expr>, lo: Option<Box<Expr>>, hi: Option<Box<Expr>> },
    Unary { op: UnOp, operand: Box<Expr> },
    /// Binary operation; operand types are equal (`lhs.ty`), except shifts
    /// where `rhs` may be any integer type.
    Binary { op: BinOp, lhs: Box<Expr>, rhs: Box<Expr> },
    /// `value == nil` (or `!=` when `negated`), for any nilable type.
    IsNil { value: Box<Expr>, negated: bool },
    /// Explicit conversion `T(x)`.
    Convert { kind: ConvKind, value: Box<Expr> },
    Call { callee: Callee, args: Vec<Expr> },
    /// Struct literal; unlisted fields are zero. Field inits are in source
    /// order and evaluated in that order.
    StructLit { struct_id: u32, fields: Vec<(u32, Expr)> },
    /// Slice literal `[a, b]` (`ty` is `[]T`).
    SliceLit(Vec<Expr>),
    /// Map literal (`ty` is `map[K]V`).
    MapLit(Vec<(Expr, Expr)>),
    /// Tuple value `(a, b)` (multiple results).
    Tuple(Vec<Expr>),
    /// Enum value: variant `variant` of enum `enum_id` with payload `args`.
    EnumLit { enum_id: u32, variant: u32, args: Vec<Expr> },
    /// Box a value into an interface (`ty` is the interface/error/any type).
    /// `value.ty` is a struct, a pointer to a struct, or any type for `any`.
    ToInterface { value: Box<Expr> },
    /// Create a closure of lifted lambda `func`, capturing by reference.
    Closure { func: FuncId, captures: Vec<CaptureSource> },
    /// `e?` / `e?(ctx)`: `value` has type `T!`, the result type is `T`. On
    /// error: wrap with `context` (a string) if present, then return the
    /// error from the current (fallible) function, running defers.
    Propagate { value: Box<Expr>, context: Option<Box<Expr>> },
    /// `try value catch err { handler }`. On success yields `T`. On error
    /// binds the error to `err` (if named) and runs `handler`. When
    /// `result_used` the handler never falls through (it ends in
    /// return/break/continue/panic); otherwise the result is void and
    /// execution continues after the handler.
    TryCatch { value: Box<Expr>, err: Option<LocalId>, handler: Block, result_used: bool },
    /// `try value else fallback`: on error, use / do `fallback`.
    TryElse { value: Box<Expr>, fallback: Box<TryFallback> },
    /// `await task`: `task.ty` is `Task<T>`, result `T`.
    Await(Box<Expr>),
    /// `task.All(t1, ..., tn)`: `Task<(T1..Tn)>`, or `Task<(U1..Un)!>` when
    /// some `Ti` is `Ui!` (fails with the first error in argument order).
    TaskAll(Vec<Expr>),
    /// Built-in operation.
    Builtin { op: Builtin, args: Vec<Expr> },
    /// Format a value as `println` would (`ty` is `string`).
    ToString(Box<Expr>),
    /// `println(...)`/`print(...)` lowered to primitive print calls (`void`).
    Print(Vec<PrintItem>),
}

/// The fallback of `try ... else`.
#[derive(Debug, Clone)]
pub enum TryFallback {
    Value(Expr),
    Break(Label),
    Continue(Label),
    Return(ReturnValue),
}

/// Call targets.
#[derive(Debug, Clone)]
pub enum Callee {
    /// Direct call of a function or method (receiver is `args[0]`).
    Func(FuncId),
    /// Runtime function.
    Extern(ExternId),
    /// Dynamic dispatch: method `method` (index in the interface's method
    /// list; `Message` is 0 for `error`) on interface value `recv`.
    Interface { recv: Box<Expr>, method: u32 },
    /// Call of a closure value (`func(...)` type).
    Value(Box<Expr>),
}

/// Unary operators (operand and result have the same type, except `Not`
/// on `bool`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnOp {
    /// Numeric negation.
    Neg,
    /// Boolean not.
    Not,
}

/// Binary operators. Arithmetic/bitwise/comparison ops apply to numeric
/// operands (`lhs.ty` tells int/uint/float and width); `Eq`/`Ne` also apply
/// to `bool`, `char`, plain enums (by tag) and pointers (by address).
/// String operations have their own variants.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Add, Sub, Mul, Div, Rem,
    BitAnd, BitOr, BitXor, Shl, Shr,
    Eq, Ne, Lt, Le, Gt, Ge,
    /// Short-circuit boolean ops.
    And, Or,
    /// `string + string`.
    StrConcat,
    /// Byte-wise string comparisons.
    StrEq, StrNe, StrLt, StrLe, StrGt, StrGe,
}

/// Conversion kinds for [`ExprKind::Convert`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConvKind {
    /// Between integer types (truncate / extend by signedness).
    IntToInt,
    IntToFloat,
    /// Truncates toward zero.
    FloatToInt,
    FloatToFloat,
    /// Plain enum to its 0-based variant ordinal (int).
    EnumToInt,
    /// `string(c)`: UTF-8 encode a char.
    CharToString,
    /// `char(i)`: integer code point to char (and `int(c)` the reverse).
    IntToChar,
    CharToInt,
    /// `*T` to `weak *T` (implicit): a weak reference to the pointee.
    PtrToWeak,
    /// Reading a `weak *T`: the `*T` (retained) or `nil` once freed.
    WeakToPtr,
}

/// Built-in operations ([`ExprKind::Builtin`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Builtin {
    /// `len(string)` in bytes.
    LenString,
    /// `len([]T)`.
    LenSlice,
    /// `len([N]T)` (constant N, but the operand is still evaluated).
    LenArray,
    /// `len(map[K]V)`.
    LenMap,
    /// `append(s, x)` -> new slice header (`ty` is `[]T`).
    Append,
    /// `delete(m, k)`.
    Delete,
    /// `panic(x)`: `args[0]` is a `string` or `error`; never returns.
    Panic,
    /// `error(msg)` / `errors.New(msg)`: new error whose message is `msg`.
    ErrorNew,
    /// `errors.As(err, &target)`: `args = [err, target]` where `target` is
    /// `*S` for a struct `S`; if some error in the chain has dynamic type
    /// `S`, copies it into `*target` and yields true.
    ErrorsAs,
}

/// One primitive print operation of a lowered `println`.
#[derive(Debug, Clone)]
pub struct PrintItem {
    pub op: PrintOp,
    /// Argument for value-printing ops (`None` for `Space`/`Newline`).
    pub arg: Option<Expr>,
}

/// Primitive print operations; runtime functions in parentheses.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PrintOp {
    /// Signed integer of any width, sign-extended to i64 (`slang_print_int`).
    Int,
    /// Unsigned integer, zero-extended to u64 (`slang_print_uint`).
    Uint,
    /// `float`/`float32` as f64 (`slang_print_float`).
    Float,
    /// (`slang_print_bool`).
    Bool,
    /// (`slang_print_rune`).
    Char,
    /// `string`; other types are wrapped in [`ExprKind::ToString`] first
    /// (`slang_print_str`).
    Str,
    /// A single space (`slang_print_space`).
    Space,
    /// `\n` (`slang_print_newline`).
    Newline,
}

impl Expr {
    /// Convenience constructor.
    pub fn new(kind: ExprKind, ty: TypeId, span: Span) -> Expr {
        Expr { kind, ty, span }
    }
}

pub mod dump;
pub mod ownership;
