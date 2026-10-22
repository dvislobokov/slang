// test: var x = try f() catch err { return } keeps x initialised on success
// status: passing
namespace spec.errors.try_catch_value

import std.io

func Parse(s string) int! {
    if s == "" {
        return error("empty")
    }
    return len(s)
}

func Use(s string) {
    var n = try Parse(s) catch err {
        println("failed:", err)
        return
    }
    println("parsed", n)
}

func main() {
    Use("abc")
    Use("")
}
