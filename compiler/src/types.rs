//! Semantic types, shared by the type checker and code generator.
//!
//! Types are interned in a `TypeTable` and referred to by `TypeId`, so
//! comparisons are cheap and recursive types (structs containing pointers
//! to themselves) are representable.

use std::collections::HashMap;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Default)]
pub struct TypeId(pub u32);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum IntKind {
    I8, I16, I32, I64,
    U8, U16, U32, U64,
}

impl IntKind {
    pub fn bits(self) -> u32 {
        use IntKind::*;
        match self { I8 | U8 => 8, I16 | U16 => 16, I32 | U32 => 32, I64 | U64 => 64 }
    }
    pub fn signed(self) -> bool {
        matches!(self, IntKind::I8 | IntKind::I16 | IntKind::I32 | IntKind::I64)
    }
}

/// Identifier of a user-declared nominal type (struct, interface, enum),
/// unique across all namespaces in a compilation.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct DeclId(pub u32);

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Type {
    Void,
    Bool,
    Int(IntKind),
    /// `float` is F64, `float32` is F32.
    Float32,
    Float64,
    Char,
    String,
    /// The built-in `error` interface (`interface error { func Message() string }`).
    Error,
    /// The empty interface `any`: holds any value together with its type info.
    Any,
    /// `nil` literal before it is unified with a pointer/slice/map/func type.
    Nil,
    Pointer(TypeId),
    /// `weak *T`: a non-owning reference to the `T` of a `*T`; the payload
    /// is the pointee `T`. Reading it yields a `*T` (or `nil` once freed).
    Weak(TypeId),
    Slice(TypeId),
    Array(TypeId, u64),
    Map(TypeId, TypeId),
    Tuple(Vec<TypeId>),
    /// `T!`: either a value of `T` (may be `Void`) or an error.
    Fallible(TypeId),
    /// `Task<T>` produced by `async func`.
    Task(TypeId),
    Func(FuncSig),
    /// User struct; the `DeclId` indexes `hir::Program::structs`.
    /// (`Interface` ids index `hir::Program::interfaces`, `Enum` ids index
    /// `hir::Program::enums`: each kind has its own id space.)
    Struct(DeclId),
    Interface(DeclId),
    Enum(DeclId),
    /// Placeholder for inference; must not survive type checking.
    Infer(u32),
    /// A generic type parameter (`T` of `func Max<T: Ordered>`), the index
    /// of the parameter in the analyser's table. Only used while a generic
    /// declaration is checked against its constraints; instantiations are
    /// monomorphised, so it never reaches a program returned by
    /// `sema::analyze`.
    Param(u32),
    /// Type of an ill-typed expression, used by the checker to suppress
    /// cascading diagnostics. Never present in a program returned by
    /// `sema::analyze` (analysis fails if any error was reported).
    Invalid,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct FuncSig {
    pub params: Vec<TypeId>,
    pub ret: TypeId,
}

/// Interned type storage.
#[derive(Debug, Default, Clone)]
pub struct TypeTable {
    types: Vec<Type>,
    index: HashMap<Type, TypeId>,
    /// Pre-interned common types, filled by `new()`.
    pub void: TypeId,
    pub bool_: TypeId,
    pub int: TypeId,
    pub float: TypeId,
    pub char_: TypeId,
    pub string: TypeId,
    pub error: TypeId,
    pub nil: TypeId,
    pub byte: TypeId,
    pub any: TypeId,
    pub invalid: TypeId,
}

impl TypeTable {
    pub fn new() -> TypeTable {
        let mut t = TypeTable::default();
        t.void = t.intern(Type::Void);
        t.bool_ = t.intern(Type::Bool);
        t.int = t.intern(Type::Int(IntKind::I64));
        t.float = t.intern(Type::Float64);
        t.char_ = t.intern(Type::Char);
        t.string = t.intern(Type::String);
        t.error = t.intern(Type::Error);
        t.nil = t.intern(Type::Nil);
        t.byte = t.intern(Type::Int(IntKind::U8));
        t.any = t.intern(Type::Any);
        t.invalid = t.intern(Type::Invalid);
        t
    }

    /// Number of interned types; valid ids are `0..len()`.
    pub fn len(&self) -> usize {
        self.types.len()
    }

    /// True if no type has been interned (never the case after `new()`).
    pub fn is_empty(&self) -> bool {
        self.types.is_empty()
    }

    pub fn intern(&mut self, ty: Type) -> TypeId {
        if let Some(&id) = self.index.get(&ty) {
            return id;
        }
        let id = TypeId(self.types.len() as u32);
        self.types.push(ty.clone());
        self.index.insert(ty, id);
        id
    }

    /// The id of `ty` if it was interned already.
    pub fn lookup(&self, ty: &Type) -> Option<TypeId> {
        self.index.get(ty).copied()
    }

    pub fn get(&self, id: TypeId) -> &Type {
        &self.types[id.0 as usize]
    }

    pub fn pointer(&mut self, to: TypeId) -> TypeId { self.intern(Type::Pointer(to)) }
    pub fn weak(&mut self, to: TypeId) -> TypeId { self.intern(Type::Weak(to)) }
    pub fn slice(&mut self, of: TypeId) -> TypeId { self.intern(Type::Slice(of)) }
    pub fn fallible(&mut self, of: TypeId) -> TypeId { self.intern(Type::Fallible(of)) }
    pub fn task(&mut self, of: TypeId) -> TypeId { self.intern(Type::Task(of)) }
    pub fn tuple(&mut self, of: Vec<TypeId>) -> TypeId { self.intern(Type::Tuple(of)) }
    pub fn map(&mut self, k: TypeId, v: TypeId) -> TypeId { self.intern(Type::Map(k, v)) }
    pub fn func(&mut self, params: Vec<TypeId>, ret: TypeId) -> TypeId {
        self.intern(Type::Func(FuncSig { params, ret }))
    }

    /// Built-in type by source name, e.g. "int", "float32", "byte".
    pub fn builtin(&mut self, name: &str) -> Option<TypeId> {
        use IntKind::*;
        Some(match name {
            "void" => self.void,
            "bool" => self.bool_,
            "int" | "int64" => self.int,
            "int8" => self.intern(Type::Int(I8)),
            "int16" => self.intern(Type::Int(I16)),
            "int32" => self.intern(Type::Int(I32)),
            "uint" | "uint64" => self.intern(Type::Int(U64)),
            "uint8" | "byte" => self.byte,
            "uint16" => self.intern(Type::Int(U16)),
            "uint32" => self.intern(Type::Int(U32)),
            "float" | "float64" => self.float,
            "float32" => self.intern(Type::Float32),
            "char" => self.char_,
            "string" => self.string,
            "error" => self.error,
            "any" => self.any,
            _ => return None,
        })
    }

    /// `[N]T` array type.
    pub fn array(&mut self, of: TypeId, n: u64) -> TypeId { self.intern(Type::Array(of, n)) }

    /// The `T` of `T!`, if `id` is fallible.
    pub fn fallible_inner(&self, id: TypeId) -> Option<TypeId> {
        match self.get(id) { Type::Fallible(t) => Some(*t), _ => None }
    }
    /// True for `T!`.
    pub fn is_fallible(&self, id: TypeId) -> bool { matches!(self.get(id), Type::Fallible(_)) }
    /// True for interface-like types: user interfaces, `error`, `any`.
    pub fn is_interface(&self, id: TypeId) -> bool { matches!(self.get(id), Type::Interface(_) | Type::Error | Type::Any) }
    /// True for `Invalid` (an earlier error was reported).
    pub fn is_invalid(&self, id: TypeId) -> bool { matches!(self.get(id), Type::Invalid) }
    /// True for signed integer types.
    pub fn is_signed(&self, id: TypeId) -> bool { matches!(self.get(id), Type::Int(k) if k.signed()) }

    pub fn is_integer(&self, id: TypeId) -> bool { matches!(self.get(id), Type::Int(_)) }
    pub fn is_float(&self, id: TypeId) -> bool { matches!(self.get(id), Type::Float32 | Type::Float64) }
    pub fn is_numeric(&self, id: TypeId) -> bool { self.is_integer(id) || self.is_float(id) }
    /// Types that may hold `nil`.
    pub fn is_nilable(&self, id: TypeId) -> bool {
        matches!(self.get(id), Type::Pointer(_) | Type::Weak(_) | Type::Slice(_) | Type::Map(..) | Type::Func(_) | Type::Interface(_) | Type::Error | Type::Any | Type::Nil)
    }

    /// Readable form for diagnostics; `names` resolves declared types.
    pub fn display(&self, id: TypeId, names: &dyn Fn(DeclId) -> String) -> String {
        use Type::*;
        match self.get(id) {
            Void => "void".into(),
            Bool => "bool".into(),
            Int(k) => match k {
                IntKind::I64 => "int".into(),
                IntKind::U8 => "byte".into(),
                k => format!("{}int{}", if k.signed() { "" } else { "u" }, k.bits()),
            },
            Float32 => "float32".into(),
            Float64 => "float".into(),
            Char => "char".into(),
            String => "string".into(),
            Error => "error".into(),
            Any => "any".into(),
            Invalid => "<invalid>".into(),
            Nil => "nil".into(),
            Pointer(t) => format!("*{}", self.display(*t, names)),
            Weak(t) => format!("weak *{}", self.display(*t, names)),
            Slice(t) => format!("[]{}", self.display(*t, names)),
            Array(t, n) => format!("[{n}]{}", self.display(*t, names)),
            Map(k, v) => format!("map[{}]{}", self.display(*k, names), self.display(*v, names)),
            Tuple(ts) => format!("({})", ts.iter().map(|t| self.display(*t, names)).collect::<Vec<_>>().join(", ")),
            Fallible(t) => format!("{}!", self.display(*t, names)),
            Task(t) => format!("Task<{}>", self.display(*t, names)),
            Func(sig) => {
                let ps = sig.params.iter().map(|t| self.display(*t, names)).collect::<Vec<_>>().join(", ");
                if *self.get(sig.ret) == Void { format!("func({ps})") } else { format!("func({ps}) {}", self.display(sig.ret, names)) }
            }
            Struct(d) | Interface(d) | Enum(d) => names(*d),
            Infer(n) => format!("?{n}"),
            Param(n) => format!("T#{n}"),
        }
    }
}
