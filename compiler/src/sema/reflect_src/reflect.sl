namespace std.reflect

// Kind of a type (TypeInfo.Kind). `int64` is `int`, `uint64` is `Uint`,
// `byte` is `Uint8`, `float64` is `Float`.
enum Kind {
    Invalid
    Void
    Bool
    Int
    Int8
    Int16
    Int32
    Uint
    Uint8
    Uint16
    Uint32
    Float
    Float32
    Char
    String
    Error
    Any
    Interface
    Struct
    Enum
    Pointer
    Weak
    Slice
    Array
    Map
    Tuple
    Func
    Fallible
    Task
}

// Declaration kinds for @attribute(targets: [...]).
enum Target {
    Namespace
    Struct
    Interface
    Enum
    Variant
    Field
    Func
    Param
}

// Values of the builtin attributes (f.Attribute<json>()).
struct json {
    name string = ""
    omitEmpty bool = false
    skip bool = false
}

struct deprecated {
    message string = ""
}

struct inline {
}

struct test {
}

// The static description of a type. The first three words are the
// runtime's slang_type_info header (runtime/ABI.md section 17).
struct TypeInfo {
    rtName int
    rtSize int
    rtDrop int
    Kind Kind
    Name string
    FullName string
    Size int
    Align int
    Fields []*FieldInfo
    Methods []*MethodInfo
    Variants []*VariantInfo
    Elem *TypeInfo
    Key *TypeInfo
    Len int
    attrs []any
    ptrTo *TypeInfo
    canon *TypeInfo

    func Attribute<A>() *A {
        return attrOf<A>(this.attrs)
    }

    func Attributes<A>() []*A {
        return attrsOf<A>(this.attrs)
    }

    func Field(name string) *FieldInfo {
        for f in this.Fields {
            if f.Name == name {
                return f
            }
        }
        return nil
    }

    func Method(name string) *MethodInfo {
        for m in this.Methods {
            if m.Name == name {
                return m
            }
        }
        return nil
    }

    func Variant(name string) *VariantInfo {
        for v in this.Variants {
            if v.Name == name {
                return v
            }
        }
        return nil
    }

    func New() any {
        if this.ptrTo == nil {
            panic("reflect: " + this.Name + ".New: no constructor metadata (name the type in reflect.TypeOf<T>(), typeof or mark it @reflect)")
        }
        return newOf(this)
    }

    func Same(other *TypeInfo) bool {
        return sameType(this, other)
    }

    func String() string {
        return this.Name
    }
}

struct FieldInfo {
    Name string
    Type *TypeInfo
    Offset int
    Index int
    Embedded bool
    Exported bool
    attrs []any

    func Attribute<A>() *A {
        return attrOf<A>(this.attrs)
    }

    func Attributes<A>() []*A {
        return attrsOf<A>(this.attrs)
    }
}

struct MethodInfo {
    Name string
    Type *TypeInfo
    Static bool
    invoke int
    fnval int
    attrs []any

    func Attribute<A>() *A {
        return attrOf<A>(this.attrs)
    }

    func Attributes<A>() []*A {
        return attrsOf<A>(this.attrs)
    }

    // m.Call(recv, args...) is lowered to this: the arguments are boxed
    // into a []any; the result (if any) is boxed into an any.
    func Invoke(recv any, args []any) any! {
        return invokeMethod(this.invoke, recv, args)?
    }

    func Call<F>() F {
        var want = TypeOf<F>()
        if want != this.Type {
            panic("reflect: " + this.Name + ".Call<" + want.Name + ">: the method has type " + this.Type.Name)
        }
        return asFunc<F>(this.fnval)
    }
}

struct VariantInfo {
    Name string
    Index int
    Fields []*FieldInfo
    attrs []any

    func Attribute<A>() *A {
        return attrOf<A>(this.attrs)
    }

    func Attributes<A>() []*A {
        return attrsOf<A>(this.attrs)
    }
}

// A reference to a value: its address, its type and an owner keeping the
// memory alive.
struct Value {
    owner any
    addr int
    Type *TypeInfo
    name string

    func IsValid() bool {
        return this.Type != nil
    }

    func Field(name string) Value {
        var t = this.Type
        if t == nil {
            panic("reflect: Field(" + name + ") of an invalid Value")
        }
        var addr = this.addr
        if t.Kind == Kind.Pointer {
            addr = loadAddr(addr)
            if addr == 0 {
                panic("reflect: Field(" + name + ") through a nil " + t.Name)
            }
            t = t.Elem
        }
        if t.Kind != Kind.Struct {
            panic("reflect: Field(" + name + ") of non-struct type " + t.Name)
        }
        var f = t.Field(name)
        if f == nil {
            panic("reflect: struct " + t.Name + " has no field " + name)
        }
        return Value{owner: this.owner, addr: addr + f.Offset, Type: f.Type, name: f.Name}
    }

    func Get<T>() T {
        var want = TypeOf<T>()
        if this.Type == nil {
            panic("reflect: Get<" + want.Name + "> of an invalid Value")
        }
        if !sameType(want, this.Type) {
            panic("reflect: Get<" + want.Name + ">: " + this.name + " has type " + this.Type.Name)
        }
        return load<T>(this.addr)
    }

    func Set<T>(v T) {
        var want = TypeOf<T>()
        if this.Type == nil {
            panic("reflect: Set of an invalid Value")
        }
        if !sameType(want, this.Type) {
            panic("reflect: cannot set " + this.name + " (type " + this.Type.Name + ") to a value of type " + want.Name)
        }
        store<T>(this.addr, v)
    }
}

func ValueOf(x any) Value {
    var t = anyType(x)
    if t == nil {
        return Value{}
    }
    return Value{owner: x, addr: anyAddr(x), Type: t, name: t.Name}
}

// Compile-time field list of struct T: only as the sequence of a for loop
// inside a generic function (unrolled at instantiation).
func Fields<T>() []*FieldInfo {
    return TypeOf<T>().Fields
}

func sameType(a *TypeInfo, b *TypeInfo) bool {
    return canonOf(a) == canonOf(b)
}

func canonOf(t *TypeInfo) *TypeInfo {
    if t != nil && t.canon != nil {
        return t.canon
    }
    return t
}

func attrOf<A>(attrs []any) *A {
    for a in attrs {
        switch a {
            case *A p:
                return p
        }
    }
    return nil
}

func attrsOf<A>(attrs []any) []*A {
    var out []*A = []
    for a in attrs {
        switch a {
            case *A p:
                out = append(out, p)
        }
    }
    return out
}

// Intrinsics: code generation provides the bodies (compiler/src/codegen/reflect.rs).

func TypeOf<T>() *TypeInfo {
    return nil
}

func anyType(x any) *TypeInfo {
    return nil
}

func anyAddr(x any) int {
    return 0
}

func ifaceType<I>(x I) *TypeInfo {
    return nil
}

func errType(e error) *TypeInfo {
    return nil
}

func loadAddr(addr int) int {
    return 0
}

func load<T>(addr int) T {
    var zero T
    return zero
}

func store<T>(addr int, v T) {
}

func newOf(t *TypeInfo) any {
    return nil
}

func invokeMethod(fn int, recv any, args []any) any! {
    return nil
}

func asFunc<F>(fn int) F {
    var zero F
    return zero
}

func storeTag<T>(p *T, tag int) {
}
