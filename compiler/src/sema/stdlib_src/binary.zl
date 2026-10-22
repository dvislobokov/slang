// std.encoding.binary: fixed-size integers in big/little endian, varints,
// and Encode/Decode/Read/Write of whole structs through compile-time
// reflection (each instance is straight-line code for its type).
//
// Byte orders as static methods, like Go: binary.BigEndian.Uint32(b),
// binary.LittleEndian.PutUint16(b, v). As a value (for Encode/Read):
// binary.ByteOrder.BigEndian / LittleEndian. A struct or field can override the order
// with the attribute @binary(endian: "big") / "little".
namespace std.encoding.binary

// Per-struct or per-field byte order: @binary(endian: "big" | "little").
@attribute(targets: [std.reflect.Target.Struct, std.reflect.Target.Field])
struct binary {
    endian string = ""
}

// A byte order as a value (for Encode/Decode/Read/Write).
enum ByteOrder {
    BigEndian
    LittleEndian
}

func get16(o ByteOrder, b []byte) uint16 {
    if o == ByteOrder.BigEndian {
        return BigEndian.Uint16(b)
    }
    return LittleEndian.Uint16(b)
}

func get32(o ByteOrder, b []byte) uint32 {
    if o == ByteOrder.BigEndian {
        return BigEndian.Uint32(b)
    }
    return LittleEndian.Uint32(b)
}

func get64(o ByteOrder, b []byte) uint {
    if o == ByteOrder.BigEndian {
        return BigEndian.Uint64(b)
    }
    return LittleEndian.Uint64(b)
}

func app16(o ByteOrder, b []byte, v uint16) []byte {
    if o == ByteOrder.BigEndian {
        return BigEndian.AppendUint16(b, v)
    }
    return LittleEndian.AppendUint16(b, v)
}

func app32(o ByteOrder, b []byte, v uint32) []byte {
    if o == ByteOrder.BigEndian {
        return BigEndian.AppendUint32(b, v)
    }
    return LittleEndian.AppendUint32(b, v)
}

func app64(o ByteOrder, b []byte, v uint) []byte {
    if o == ByteOrder.BigEndian {
        return BigEndian.AppendUint64(b, v)
    }
    return LittleEndian.AppendUint64(b, v)
}

func need(b []byte, n int) {
    if len(b) < n {
        panic("binary: buffer too short")
    }
}

// Big-endian (network order) access to byte slices.
struct BigEndian {
    // Uint16 decodes b[0..2]; it panics if b is shorter.
    static func Uint16(b []byte) uint16 {
        need(b, 2)
        return (uint16(b[0]) << 8) | uint16(b[1])
    }

    // Uint32 decodes b[0..4]; it panics if b is shorter.
    static func Uint32(b []byte) uint32 {
        need(b, 4)
        return (uint32(b[0]) << 24) | (uint32(b[1]) << 16) | (uint32(b[2]) << 8) | uint32(b[3])
    }

    // Uint64 decodes b[0..8]; it panics if b is shorter.
    static func Uint64(b []byte) uint {
        need(b, 8)
        var v uint = 0
        for i in 0..8 {
            v = (v << 8) | uint(b[i])
        }
        return v
    }

    // PutUint16 encodes v into b[0..2]; it panics if b is shorter.
    static func PutUint16(b []byte, v uint16) {
        need(b, 2)
        b[0] = byte(v >> 8)
        b[1] = byte(v & 255)
    }

    // PutUint32 encodes v into b[0..4]; it panics if b is shorter.
    static func PutUint32(b []byte, v uint32) {
        need(b, 4)
        b[0] = byte(v >> 24)
        b[1] = byte((v >> 16) & 255)
        b[2] = byte((v >> 8) & 255)
        b[3] = byte(v & 255)
    }

    // PutUint64 encodes v into b[0..8]; it panics if b is shorter.
    static func PutUint64(b []byte, v uint) {
        need(b, 8)
        for i in 0..8 {
            b[i] = byte((v >> uint(56 - 8 * i)) & 255)
        }
    }

    // AppendUint16 appends the 2 bytes of v to b.
    static func AppendUint16(b []byte, v uint16) []byte {
        var out = append(b, byte(v >> 8))
        return append(out, byte(v & 255))
    }

    // AppendUint32 appends the 4 bytes of v to b.
    static func AppendUint32(b []byte, v uint32) []byte {
        var out = b
        var shift = 24
        for shift >= 0 {
            out = append(out, byte((v >> uint32(shift)) & 255))
            shift -= 8
        }
        return out
    }

    // AppendUint64 appends the 8 bytes of v to b.
    static func AppendUint64(b []byte, v uint) []byte {
        var out = b
        var shift = 56
        for shift >= 0 {
            out = append(out, byte((v >> uint(shift)) & 255))
            shift -= 8
        }
        return out
    }
}

// Little-endian access to byte slices.
struct LittleEndian {
    // Uint16 decodes b[0..2]; it panics if b is shorter.
    static func Uint16(b []byte) uint16 {
        need(b, 2)
        return uint16(b[0]) | (uint16(b[1]) << 8)
    }

    // Uint32 decodes b[0..4]; it panics if b is shorter.
    static func Uint32(b []byte) uint32 {
        need(b, 4)
        return uint32(b[0]) | (uint32(b[1]) << 8) | (uint32(b[2]) << 16) | (uint32(b[3]) << 24)
    }

    // Uint64 decodes b[0..8]; it panics if b is shorter.
    static func Uint64(b []byte) uint {
        need(b, 8)
        var v uint = 0
        var i = 7
        for i >= 0 {
            v = (v << 8) | uint(b[i])
            i -= 1
        }
        return v
    }

    // PutUint16 encodes v into b[0..2]; it panics if b is shorter.
    static func PutUint16(b []byte, v uint16) {
        need(b, 2)
        b[0] = byte(v & 255)
        b[1] = byte(v >> 8)
    }

    // PutUint32 encodes v into b[0..4]; it panics if b is shorter.
    static func PutUint32(b []byte, v uint32) {
        need(b, 4)
        for i in 0..4 {
            b[i] = byte((v >> uint32(8 * i)) & 255)
        }
    }

    // PutUint64 encodes v into b[0..8]; it panics if b is shorter.
    static func PutUint64(b []byte, v uint) {
        need(b, 8)
        for i in 0..8 {
            b[i] = byte((v >> uint(8 * i)) & 255)
        }
    }

    // AppendUint16 appends the 2 bytes of v to b.
    static func AppendUint16(b []byte, v uint16) []byte {
        var out = append(b, byte(v & 255))
        return append(out, byte(v >> 8))
    }

    // AppendUint32 appends the 4 bytes of v to b.
    static func AppendUint32(b []byte, v uint32) []byte {
        var out = b
        for i in 0..4 {
            out = append(out, byte((v >> uint32(8 * i)) & 255))
        }
        return out
    }

    // AppendUint64 appends the 8 bytes of v to b.
    static func AppendUint64(b []byte, v uint) []byte {
        var out = b
        for i in 0..8 {
            out = append(out, byte((v >> uint(8 * i)) & 255))
        }
        return out
    }
}

// Appends the unsigned varint (LEB128) encoding of x.
func AppendUvarint(b []byte, x uint) []byte {
    var out = b
    var v = x
    for v >= 0x80 {
        out = append(out, byte((v & 0x7F) | 0x80))
        v = v >> 7
    }
    return append(out, byte(v))
}

// Appends the zig-zag signed varint encoding of x.
func AppendVarint(b []byte, x int) []byte {
    var ux = uint(x) << 1
    if x < 0 {
        ux = ux ^ 18446744073709551615
    }
    return AppendUvarint(b, ux)
}

// Writes the uvarint of x into b (which must be long enough, at most
// MaxVarintLen64 bytes) and returns the number of bytes written.
func PutUvarint(b []byte, x uint) int {
    var enc []byte = []
    enc = AppendUvarint(enc, x)
    need(b, len(enc))
    for i, c in enc {
        b[i] = c
    }
    return len(enc)
}

// Writes the varint of x into b; returns the number of bytes written.
func PutVarint(b []byte, x int) int {
    var enc []byte = []
    enc = AppendVarint(enc, x)
    need(b, len(enc))
    for i, c in enc {
        b[i] = c
    }
    return len(enc)
}

// Decodes a uvarint from b: (value, bytes read); n == 0 if b is too
// short, n < 0 (-bytes read) on 64-bit overflow, as in Go.
func Uvarint(b []byte) (uint, int) {
    var x uint = 0
    var s uint = 0
    for i, c in b {
        if i == 10 {
            return (0, -(i + 1))
        }
        if c < 0x80 {
            if i == 9 && c > 1 {
                return (0, -(i + 1))
            }
            return (x | (uint(c) << s), i + 1)
        }
        x = x | (uint(c & 0x7F) << s)
        s += 7
    }
    return (0, 0)
}

// Decodes a zig-zag varint from b: (value, bytes read), n as in Uvarint.
func Varint(b []byte) (int, int) {
    var (ux, n) = Uvarint(b)
    var x = int(ux >> 1)
    if (ux & 1) != 0 {
        x = int((ux >> 1) ^ 18446744073709551615)
    }
    return (x, n)
}

func orderOf(tag *binary, def ByteOrder) ByteOrder {
    if tag == nil {
        return def
    }
    if tag.endian == "big" {
        return ByteOrder.BigEndian
    }
    if tag.endian == "little" {
        return ByteOrder.LittleEndian
    }
    if tag.endian != "" {
        panic("binary: @binary(endian:) must be \"big\" or \"little\", not \"" + tag.endian + "\"")
    }
    return def
}

// The encoded size of a T: fixed-size integers, floats, bool (1 byte),
// arrays and structs of those. Panics for other types.
func Size<T>() int {
    var z T
    return len(Encode(z, ByteOrder.BigEndian))
}

// The encoding of v: integers and floats in order (floats as IEEE bits
// via float bits of the same width), bool as one byte, fixed arrays
// element by element, structs field by field (each may override the
// order with @binary). int and uint are 8 bytes.
func Encode<T>(v T, order ByteOrder) []byte {
    var out []byte = []
    return encodeTo(out, v, order)
}

func encodeTo<T>(b []byte, v T, order ByteOrder) []byte {
    var out = b
    if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Struct {
        var o = orderOf(std.reflect.TypeOf<T>().Attribute<binary>(), order)
        for f in std.reflect.Fields<T>() {
            var fo = orderOf(f.Attribute<binary>(), o)
            out = encodeTo(out, f.Get(v), fo)
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Array {
        for x in v {
            out = encodeTo(out, x, order)
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Bool {
        if v {
            out = append(out, 1)
        } else {
            out = append(out, 0)
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint8 {
        out = append(out, v)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int8 {
        out = append(out, byte(v))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint16 ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int16 {
        out = app16(order, out, uint16(v))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint32 ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int32 ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Char {
        out = app32(order, out, uint32(v))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int {
        out = app64(order, out, uint(v))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Float {
        out = app64(order, out, std.math.Float64bits(v))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Float32 {
        out = app32(order, out, std.math.Float32bits(v))
    } else {
        panic(
            "binary: cannot encode type " +
                std.reflect.TypeOf<T>().Name +
                " (fixed-size types only)",
        )
    }
    return out
}

// Decodes a T from the start of b (see Encode); error if b is too short.
func Decode<T>(b []byte, order ByteOrder) T! {
    var out T
    var pos = 0
    decodeFrom(b, &pos, order, &out)?
    return out
}

func take(b []byte, pos *int, n int) []byte! {
    if *pos + n > len(b) {
        return error("binary: unexpected end of data")
    }
    var s = b[*pos..*pos + n]
    *pos = *pos + n
    return s
}

func decodeFrom<T>(b []byte, pos *int, order ByteOrder, out *T) void! {
    if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Struct {
        var o = orderOf(std.reflect.TypeOf<T>().Attribute<binary>(), order)
        for f in std.reflect.Fields<T>() {
            var fo = orderOf(f.Attribute<binary>(), o)
            decodeFrom(b, pos, fo, &f.Get(*out))?
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Array {
        for i in 0..len(*out) {
            decodeFrom(b, pos, order, &(*out)[i])?
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Bool {
        *out = take(b, pos, 1)?[0] != 0
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint8 {
        *out = take(b, pos, 1)?[0]
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int8 {
        *out = int8(take(b, pos, 1)?[0])
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint16 {
        *out = get16(order, take(b, pos, 2)?)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int16 {
        *out = int16(get16(order, take(b, pos, 2)?))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint32 {
        *out = get32(order, take(b, pos, 4)?)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int32 {
        *out = int32(get32(order, take(b, pos, 4)?))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Char {
        *out = char(int32(get32(order, take(b, pos, 4)?)))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint {
        *out = get64(order, take(b, pos, 8)?)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int {
        *out = int(get64(order, take(b, pos, 8)?))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Float {
        *out = std.math.Float64frombits(get64(order, take(b, pos, 8)?))
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Float32 {
        *out = std.math.Float32frombits(get32(order, take(b, pos, 4)?))
    } else {
        panic(
            "binary: cannot decode type " +
                std.reflect.TypeOf<T>().Name +
                " (fixed-size types only)",
        )
    }
}

// Reads Size<T>() bytes from r and decodes them (io.ReadFull semantics).
func Read<T>(r std.io.Reader, order ByteOrder) T! {
    var n = Size<T>()
    var buf = std.io.ReadN(r, n)?
    return Decode<T>(buf, order)?
}

// Writes the encoding of v to w.
func Write<T>(w std.io.Writer, order ByteOrder, v T) void! {
    var b = Encode(v, order)
    _ = std.io.WriteAll(w, b)?
}
