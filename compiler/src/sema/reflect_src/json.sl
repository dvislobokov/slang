namespace std.encoding.json

// json.Format / json.Parse use compile-time reflection: inside these generic
// functions `reflect.Fields<T>()` loops are unrolled per field and the
// `if reflect.TypeOf<T>().Kind == ...` tests are resolved at compile time,
// so every instance is straight-line code for its type. Library code names
// other namespaces by their full path (it is instantiated in user scope).

// A dynamic JSON value (json.ParseValue).
struct Value {
    kind int
    b bool
    n float
    s string
    items []Value
    keys []string

    func IsNull() bool {
        return this.kind == 0
    }

    func IsBool() bool {
        return this.kind == 1
    }

    func IsNumber() bool {
        return this.kind == 2
    }

    func IsString() bool {
        return this.kind == 3
    }

    func IsArray() bool {
        return this.kind == 4
    }

    func IsObject() bool {
        return this.kind == 5
    }

    // "null", "bool", "number", "string", "array" or "object".
    func Type() string {
        return kindName(this.kind)
    }

    func Bool() bool! {
        if this.kind != 1 {
            return error("json: " + kindName(this.kind) + " is not a bool")
        }
        return this.b
    }

    func Float() float! {
        if this.kind != 2 {
            return error("json: " + kindName(this.kind) + " is not a number")
        }
        return this.n
    }

    func Int() int! {
        if this.kind != 2 {
            return error("json: " + kindName(this.kind) + " is not a number")
        }
        return integral(this.n, "")?
    }

    func String() string! {
        if this.kind != 3 {
            return error("json: " + kindName(this.kind) + " is not a string")
        }
        return this.s
    }

    // Number of elements of an array or members of an object, else 0.
    func Len() int {
        if this.kind == 4 || this.kind == 5 {
            return len(this.items)
        }
        return 0
    }

    // Element i of an array (null if out of range or not an array).
    func Index(i int) Value {
        if this.kind == 4 && i >= 0 && i < len(this.items) {
            return this.items[i]
        }
        return Value{}
    }

    // Member `key` of an object (null if missing or not an object; the
    // last one wins for duplicate keys).
    func Get(key string) Value {
        var i = this.find(key)
        if i < 0 {
            return Value{}
        }
        return this.items[i]
    }

    func Has(key string) bool {
        return this.find(key) >= 0
    }

    func find(key string) int {
        var found = -1
        if this.kind == 5 {
            for i, k in this.keys {
                if k == key {
                    found = i
                }
            }
        }
        return found
    }

    // Member names of an object in document order.
    func Keys() []string {
        var out []string = []
        if this.kind == 5 {
            for k in this.keys {
                out = append(out, k)
            }
        }
        return out
    }

    // Elements of an array (or member values of an object).
    func Items() []Value {
        var out []Value = []
        for x in this.items {
            out = append(out, x)
        }
        return out
    }

    // Compact JSON text of the value.
    func Format() string {
        var b = std.strings.Builder{}
        writeValue(&b, *this)
        return b.String()
    }
}

func NewNull() Value {
    return Value{}
}

func NewBool(b bool) Value {
    return Value{kind: 1, b: b}
}

func NewNumber(n float) Value {
    return Value{kind: 2, n: n}
}

func NewString(s string) Value {
    return Value{kind: 3, s: s}
}

func NewArray(items []Value) Value {
    var copy []Value = []
    for x in items {
        copy = append(copy, x)
    }
    return Value{kind: 4, items: copy}
}

func NewObject(keys []string, values []Value) Value! {
    if len(keys) != len(values) {
        return error("json.NewObject: keys and values differ in length")
    }
    var ks []string = []
    var vs []Value = []
    for i, k in keys {
        ks = append(ks, k)
        vs = append(vs, values[i])
    }
    return Value{kind: 5, keys: ks, items: vs}
}

func kindName(k int) string {
    if k == 0 {
        return "null"
    }
    if k == 1 {
        return "bool"
    }
    if k == 2 {
        return "number"
    }
    if k == 3 {
        return "string"
    }
    if k == 4 {
        return "array"
    }
    return "object"
}

// -- parsing ----------------------------------------------------------------

// Parses a JSON text into a dynamic Value.
func ParseValue(text string) Value! {
    var p = parser{toks: scan(text)?}
    var v = p.value(0)?
    if p.pos < len(p.toks) {
        return error("json: unexpected " + show(p.toks[p.pos]) + " after the top-level value")
    }
    return v
}

struct parser {
    toks []string
    pos int

    func next() string! {
        if this.pos >= len(this.toks) {
            return error("json: unexpected end of input")
        }
        var t = this.toks[this.pos]
        this.pos += 1
        return t
    }

    func peek() string {
        if this.pos < len(this.toks) {
            return this.toks[this.pos]
        }
        return ""
    }

    func value(depth int) Value! {
        if depth > 512 {
            return error("json: nesting too deep")
        }
        var t = this.next()?
        var c = t[0..1]
        if c == "s" {
            return Value{kind: 3, s: t[1..]}
        }
        if c == "n" {
            var n = std.strings.ToFloat(t[1..])?
            return Value{kind: 2, n: n}
        }
        if t == "t" {
            return Value{kind: 1, b: true}
        }
        if t == "f" {
            return Value{kind: 1}
        }
        if t == "z" {
            return Value{}
        }
        if t == "[" {
            var items []Value = []
            if this.peek() == "]" {
                this.pos += 1
                return Value{kind: 4, items: items}
            }
            for {
                var x = this.value(depth + 1)?
                items = append(items, x)
                var sep = this.next()?
                if sep == "]" {
                    break
                }
                if sep != "," {
                    return error("json: expected , or ] in an array, found " + show(sep))
                }
            }
            return Value{kind: 4, items: items}
        }
        if t == "{" {
            var keys []string = []
            var items []Value = []
            if this.peek() == "}" {
                this.pos += 1
                return Value{kind: 5, keys: keys, items: items}
            }
            for {
                var k = this.next()?
                if k[0..1] != "s" {
                    return error("json: expected a string key in an object, found " + show(k))
                }
                var colon = this.next()?
                if colon != ":" {
                    return error("json: expected : after an object key, found " + show(colon))
                }
                var x = this.value(depth + 1)?
                keys = append(keys, k[1..])
                items = append(items, x)
                var sep = this.next()?
                if sep == "}" {
                    break
                }
                if sep != "," {
                    return error("json: expected , or } in an object, found " + show(sep))
                }
            }
            return Value{kind: 5, keys: keys, items: items}
        }
        return error("json: unexpected " + show(t))
    }
}

// A token for messages.
func show(t string) string {
    var c = t[0..1]
    if c == "s" {
        return "string " + Quote(t[1..])
    }
    if c == "n" {
        return "number " + t[1..]
    }
    if t == "t" {
        return "true"
    }
    if t == "f" {
        return "false"
    }
    if t == "z" {
        return "null"
    }
    return "'" + t + "'"
}

// Parses a JSON text into a T (structs by their fields and @json, slices,
// maps with string keys, pointers, strings, numbers, bools, plain enums by
// variant name, json.Value, any). Errors name the path: "users[2].id: ...".
func Parse<T>(text string) T! {
    var v = ParseValue(text)?
    var out T
    decode(v, "", &out)?
    return out
}

// Decodes a dynamic Value into a T.
func Decode<T>(v Value) T! {
    var out T
    decode(v, "", &out)?
    return out
}

func prefix(path string) string {
    if path == "" {
        return "json: "
    }
    return path + ": "
}

func join(path string, name string) string {
    if path == "" {
        return name
    }
    return path + "." + name
}

func mismatch(path string, want string, v Value) error {
    return error(prefix(path) + "expected " + want + ", found " + kindName(v.kind))
}

// The integer value of n (an error if it has a fraction or is too large).
func integral(n float, path string) int! {
    if n != std.math.Trunc(n) || n < -9223372036854775808.0 || n >= 9223372036854775808.0 {
        return error(prefix(path) + "expected int, found number " + std.fmt.Format("{0}", n))
    }
    return int(n)
}

func intIn(v Value, path string, lo int, hi int) int! {
    if v.kind != 2 {
        return mismatch(path, "int", v)
    }
    var n = integral(v.n, path)?
    if n < lo || n > hi {
        return error(prefix(path) + "number " + std.fmt.Format("{0}", n) + " out of range")
    }
    return n
}

func decode<T>(v Value, path string, out *T) void! {
    if std.reflect.TypeOf<T>() == std.reflect.TypeOf<Value>() {
        *out = v
        return
    }
    if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Pointer || std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Slice || std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Map {
        if v.kind == 0 {
            *out = nil
            return
        }
    }
    if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Struct {
        if v.kind != 5 {
            return mismatch(path, "object", v)
        }
        for f in std.reflect.Fields<T>() {
            var tag = f.Attribute<std.reflect.json>()
            if tag != nil && tag.skip {
                continue
            }
            var name = f.Name
            if tag != nil && tag.name != "" {
                name = tag.name
            }
            var i = v.find(name)
            if i >= 0 {
                decode(v.items[i], join(path, name), &f.Get(*out))?
            }
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.String {
        if v.kind != 3 {
            return mismatch(path, "string", v)
        }
        *out = v.s
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Bool {
        if v.kind != 1 {
            return mismatch(path, "bool", v)
        }
        *out = v.b
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int {
        *out = intIn(v, path, std.math.MinInt, std.math.MaxInt)?
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int8 {
        *out = int8(intIn(v, path, -128, 127)?)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int16 {
        *out = int16(intIn(v, path, -32768, 32767)?)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int32 {
        *out = int32(intIn(v, path, std.math.MinInt32, std.math.MaxInt32)?)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint {
        *out = uint(intIn(v, path, 0, std.math.MaxInt)?)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint8 {
        *out = byte(intIn(v, path, 0, 255)?)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint16 {
        *out = uint16(intIn(v, path, 0, 65535)?)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint32 {
        *out = uint32(intIn(v, path, 0, std.math.MaxUint32)?)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Float {
        if v.kind != 2 {
            return mismatch(path, "number", v)
        }
        *out = v.n
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Float32 {
        if v.kind != 2 {
            return mismatch(path, "number", v)
        }
        *out = float32(v.n)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Enum {
        if v.kind != 3 {
            return mismatch(path, "string", v)
        }
        var t = std.reflect.TypeOf<T>()
        var variant = t.Variant(v.s)
        if variant == nil || len(variant.Fields) > 0 {
            return error(prefix(path) + "unknown " + t.Name + " value " + Quote(v.s))
        }
        std.reflect.storeTag(out, variant.Index)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Slice {
        if v.kind != 4 {
            return mismatch(path, "array", v)
        }
        decodeSlice(v.items, path, out)?
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Map {
        if std.reflect.TypeOf<T>().Key.Kind == std.reflect.Kind.String {
            if v.kind != 5 {
                return mismatch(path, "object", v)
            }
            decodeMap(v, path, out)?
        } else {
            return error(prefix(path) + "cannot decode into " + std.reflect.TypeOf<T>().Name + " (map keys must be strings)")
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Pointer {
        decodePtr(v, path, out)?
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Any {
        *out = v
    } else {
        return error(prefix(path) + "cannot decode into " + std.reflect.TypeOf<T>().Name)
    }
}

func decodeSlice<E>(items []Value, path string, out *[]E) void! {
    var res []E = []
    for i, x in items {
        var e E
        decode(x, path + "[" + std.fmt.Format("{0}", i) + "]", &e)?
        res = append(res, e)
    }
    *out = res
}

func decodeMap<V>(v Value, path string, out *map[string]V) void! {
    var m = map[string]V{}
    for i, k in v.keys {
        var e V
        decode(v.items[i], join(path, k), &e)?
        m[k] = e
    }
    *out = m
}

func decodePtr<E>(v Value, path string, out **E) void! {
    var e E
    decode(v, path, &e)?
    *out = &e
}

// -- formatting -------------------------------------------------------------

// Compact JSON text of v (structs by their fields and @json(name, skip,
// omitEmpty), slices and arrays, maps (keys sorted), pointers, strings,
// numbers, bools, plain enums by variant name, json.Value, any holding
// those).
func Format<T>(v T) string {
    var b = std.strings.Builder{}
    write(&b, v)
    return b.String()
}

func write<T>(b *std.strings.Builder, v T) {
    if std.reflect.TypeOf<T>() == std.reflect.TypeOf<Value>() {
        writeValue(b, v)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Struct {
        b.Write("{")
        var first = true
        for f in std.reflect.Fields<T>() {
            var tag = f.Attribute<std.reflect.json>()
            if tag != nil && tag.skip {
                continue
            }
            var name = f.Name
            if tag != nil && tag.name != "" {
                name = tag.name
            }
            if tag != nil && tag.omitEmpty && isEmpty(f.Get(v)) {
                continue
            }
            if !first {
                b.Write(",")
            }
            first = false
            b.Write(Quote(name))
            b.Write(":")
            write(b, f.Get(v))
        }
        b.Write("}")
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.String {
        b.Write(Quote(v))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Bool {
        if v {
            b.Write("true")
        } else {
            b.Write("false")
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Float || std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Float32 {
        writeFloat(b, float(v))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int || std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int8 || std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int16 || std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int32 || std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint || std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint8 || std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint16 || std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint32 {
        b.Write(std.fmt.Format("{0}", v))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Char {
        b.Write(Quote(string(v)))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Enum {
        b.Write(Quote(std.fmt.Format("{0}", v)))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Slice {
        if v == nil {
            b.Write("null")
        } else {
            b.Write("[")
            for i, x in v {
                if i > 0 {
                    b.Write(",")
                }
                write(b, x)
            }
            b.Write("]")
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Array {
        b.Write("[")
        for i, x in v {
            if i > 0 {
                b.Write(",")
            }
            write(b, x)
        }
        b.Write("]")
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Map {
        if v == nil {
            b.Write("null")
        } else {
            var keys []string = []
            var vals []string = []
            for k, x in v {
                keys = append(keys, keyString(k))
                var sub = std.strings.Builder{}
                write(&sub, x)
                vals = append(vals, sub.String())
            }
            sortPairs(keys, vals)
            b.Write("{")
            for i, k in keys {
                if i > 0 {
                    b.Write(",")
                }
                b.Write(Quote(k))
                b.Write(":")
                b.Write(vals[i])
            }
            b.Write("}")
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Pointer {
        if v == nil {
            b.Write("null")
        } else {
            write(b, *v)
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Any {
        writeAny(b, v)
    } else {
        b.Write("null")
    }
}

func keyString<K>(k K) string {
    if std.reflect.TypeOf<K>().Kind == std.reflect.Kind.String {
        return k
    } else {
        return std.fmt.Format("{0}", k)
    }
}

// Insertion sort of keys, moving vals along.
func sortPairs(keys []string, vals []string) {
    for i in 1..len(keys) {
        var j = i
        for j > 0 && keys[j - 1] > keys[j] {
            var k = keys[j]
            keys[j] = keys[j - 1]
            keys[j - 1] = k
            var x = vals[j]
            vals[j] = vals[j - 1]
            vals[j - 1] = x
            j -= 1
        }
    }
}

func isEmpty<F>(x F) bool {
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.String {
        return x == ""
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Bool {
        return !x
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int8 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int16 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int32 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint8 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint16 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint32 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float32 {
        return x == 0
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Slice || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Map {
        return len(x) == 0
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Pointer {
        return x == nil
    } else {
        return false
    }
}

func writeFloat(b *std.strings.Builder, f float) {
    if std.math.IsNaN(f) || std.math.IsInf(f, 0) {
        b.Write("null")
    } else {
        b.Write(std.fmt.Format("{0}", f))
    }
}

func writeAny(b *std.strings.Builder, x any) {
    switch x {
        case string s:
            b.Write(Quote(s))
        case int n:
            b.Write(std.fmt.Format("{0}", n))
        case float f:
            writeFloat(b, f)
        case bool t:
            if t {
                b.Write("true")
            } else {
                b.Write("false")
            }
        case Value v:
            writeValue(b, v)
        default:
            b.Write("null")
    }
}

func writeValue(b *std.strings.Builder, v Value) {
    if v.kind == 0 {
        b.Write("null")
    } else if v.kind == 1 {
        if v.b {
            b.Write("true")
        } else {
            b.Write("false")
        }
    } else if v.kind == 2 {
        writeFloat(b, v.n)
    } else if v.kind == 3 {
        b.Write(Quote(v.s))
    } else if v.kind == 4 {
        b.Write("[")
        for i, x in v.items {
            if i > 0 {
                b.Write(",")
            }
            writeValue(b, x)
        }
        b.Write("]")
    } else {
        b.Write("{")
        for i, k in v.keys {
            if i > 0 {
                b.Write(",")
            }
            b.Write(Quote(k))
            b.Write(":")
            writeValue(b, v.items[i])
        }
        b.Write("}")
    }
}
