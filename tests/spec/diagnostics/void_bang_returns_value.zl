// error: a void! function can only return an error
// status: passing
namespace spec.diagnostics.void_bang_returns_value

import std.io

func F() void! {
    return 1
}

func main() {
    try F() catch err { println(err) }
}
