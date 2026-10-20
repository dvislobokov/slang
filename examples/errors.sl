namespace examples.errors

import std.io
import std

// Sentinel errors: compared with errors.Is.
const ErrNotFound = error("not found")
const ErrEmpty = error("empty input")

// Typed errors: any struct with Message() string is an error.
struct ValidationError {
    Field string
    Reason string

    func Message() string {
        return this.Field + ": " + this.Reason
    }
}

struct NotFound {
    Key string

    func Message() string {
        return "no such key: " + this.Key
    }
}

var store = map[string]int{"a": 1, "b": 2}

func Half(n int) int! {
    if n % 2 != 0 {
        return error(fmt.Format("{0} is odd", n))
    }
    return n / 2
}

// ? propagates; ?("context") wraps the error with a message.
func Quarter(n int) int! {
    var h = Half(n)?
    return Half(h)?("halving again")
}

func ParsePositive(s string) int! {
    if s == "" {
        return ErrEmpty
    }
    var n = strings.ToInt(s)?
    if n <= 0 {
        return ValidationError{Field: "value", Reason: "must be positive"}
    }
    return n
}

// void! : no value, but may fail.
func Validate(age int) void! {
    if age < 0 {
        return ValidationError{Field: "age", Reason: "negative"}
    }
    if age > 150 {
        return ValidationError{Field: "age", Reason: "too large"}
    }
}

func FindUser(id int) string! {
    switch id {
        case 1:
            return "alice"
        case 2:
            return "bob"
    }
    return ErrNotFound
}

func LoadProfile(id int) string! {
    var name = FindUser(id)?("loading profile")
    return "profile of " + name
}

func Lookup(key string) int! {
    var v = store[key]
    if v == 0 {
        return NotFound{Key: key}
    }
    return v
}

func Explain(err error) {
    switch err {
        case NotFound e:
            println("  missing key", e.Key)
        case ValidationError e:
            println("  bad field", e.Field, "-", e.Reason)
        default:
            println("  other error:", err)
    }
}

func Noisy() int! {
    println("Noisy called")
    return error("ignored")
}

func main() {
    // try ... catch with an early return keeps q initialised.
    var q = try Quarter(8) catch err {
        println("unexpected:", err)
        return
    }
    println("Quarter(8):", q)

    // try ... catch as a statement.
    try Quarter(6) catch err {
        println("Quarter(6) failed:", err)
    }
    try Quarter(5) catch err {
        println("Quarter(5) failed:", err)
    }

    // try ... else: default value or loop control.
    var h = try Half(7) else -1
    println("Half(7) or default:", h)
    var total = 0
    for s in ["4", "x", "", "-3", "10"] {
        var n = try ParsePositive(s) else continue
        total += n
    }
    println("sum of valid inputs:", total)

    // void! functions.
    for age in [30, -1, 200] {
        try Validate(age) catch err {
            println("invalid age", age, "->", err)
            continue
        }
        println("valid age", age)
    }

    // Typed errors and type switches.
    for key in ["a", "zz"] {
        var v = try Lookup(key) catch err {
            println("lookup", key, "failed:", err)
            Explain(err)
            continue
        }
        println("lookup", key, "=", v)
    }
    for s in ["", "0", "12"] {
        var v = try ParsePositive(s) catch err {
            println("parse", "\"" + s + "\"", "failed:", err)
            Explain(err)
            continue
        }
        println("parsed", v)
    }

    // errors.Is sees through wrapping; errors.Unwrap peels one layer.
    var p = try LoadProfile(1) catch err {
        println("unexpected:", err)
        return
    }
    println(p)
    try LoadProfile(3) catch err {
        println("error:", err)
        println("is ErrNotFound:", errors.Is(err, ErrNotFound))
        println("is ErrEmpty:", errors.Is(err, ErrEmpty))
        var inner = errors.Unwrap(err)
        println("unwrapped:", inner)
        println("unwrapped is ErrNotFound:", errors.Is(inner, ErrNotFound))
    }

    // errors.Wrap and errors.As.
    var wrapped = errors.Wrap(ValidationError{Field: "email", Reason: "missing @"}, "signup")
    println(wrapped)
    var ve ValidationError
    if errors.As(wrapped, &ve) {
        println("As found field:", ve.Field)
    }
    var nf NotFound
    println("As NotFound:", errors.As(wrapped, &nf))
    var twice = errors.Wrap(wrapped, "request")
    println(twice)
    println("twice is ErrNotFound:", errors.Is(twice, ErrNotFound))
    println("unwrap twice:", errors.Unwrap(errors.Unwrap(twice)))
    println("unwrap plain:", errors.Unwrap(ErrEmpty))
    println(errors.New("made with errors.New"))

    // Explicitly discard both the value and the error.
    _ = Noisy()
    println("done")
}
