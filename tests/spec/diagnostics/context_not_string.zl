// error: type mismatch: expected string, found int
// status: passing
namespace spec.diagnostics.context_not_string

import std.io

func F() int! { return error("x") }

func G() int! {
    return F()?(42)
}

func main() {
    println(try G() else 0)
}
