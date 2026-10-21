// Shared helpers of the reflection-based codecs std.encoding.{csv,xml,
// yaml,toml} (internal; lowercase).
namespace std.encoding

// v converted to the integer or float type F, or an error naming the
// range when it does not fit.
func intAs<F>(v int) F! {
    var lo = 0
    var hi = 0
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int {
        return v
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int8 {
        lo = -128
        hi = 127
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int16 {
        lo = -32768
        hi = 32767
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int32 {
        lo = std.math.MinInt32
        hi = std.math.MaxInt32
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint {
        lo = 0
        hi = std.math.MaxInt
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint8 {
        lo = 0
        hi = 255
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint16 {
        lo = 0
        hi = 65535
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint32 {
        lo = 0
        hi = std.math.MaxUint32
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float {
        return float(v)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float32 {
        return float32(v)
    }
    if v < lo || v > hi {
        return error(
            std.fmt.Format("number {0} out of range for {1}", v, std.reflect.TypeOf<F>().Name),
        )
    }
    return narrow<F>(v)
}

func narrow<F>(v int) F {
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int8 {
        return int8(v)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int16 {
        return int16(v)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int32 {
        return int32(v)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint {
        return uint(v)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint8 {
        return byte(v)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint16 {
        return uint16(v)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint32 {
        return uint32(v)
    } else {
        var zero F
        return zero
    }
}

// v converted to the float type F.
func floatAs<F>(v float) F {
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float32 {
        return float32(v)
    } else {
        return v
    }
}

func isIntKind<F>() bool {
    var k = std.reflect.TypeOf<F>().Kind
    return k == std.reflect.Kind.Int ||
        k == std.reflect.Kind.Int8 ||
        k == std.reflect.Kind.Int16 ||
        k == std.reflect.Kind.Int32 ||
        k == std.reflect.Kind.Uint ||
        k == std.reflect.Kind.Uint8 ||
        k == std.reflect.Kind.Uint16 ||
        k == std.reflect.Kind.Uint32
}

// The zero value test of omitEmpty.
func isEmpty<F>(x F) bool {
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.String {
        return x == ""
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Bool {
        return !x
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int ||
        std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int8 ||
        std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int16 ||
        std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int32 ||
        std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint ||
        std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint8 ||
        std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint16 ||
        std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint32 ||
        std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float ||
        std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float32 {
        return x == 0
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Slice ||
        std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Map {
        return len(x) == 0
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Pointer {
        return x == nil
    } else {
        return false
    }
}
