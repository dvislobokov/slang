// error: result of type void! must be handled
// status: passing
namespace spec.errors.unhandled_void_result_error

import std.io

func F() void! {}

func main() {
    F()
    println("x")
}
