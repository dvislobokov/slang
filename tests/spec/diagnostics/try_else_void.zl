// error: try ... else value needs a function with a result
// status: passing
namespace spec.diagnostics.try_else_void

import std.io

func F() void! { return error("x") }

func main() {
    try F() else 1
}
