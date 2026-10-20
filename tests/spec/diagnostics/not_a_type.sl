// error: is not a type
// status: passing
namespace spec.diagnostics.not_a_type

import std.io

func Helper() {}

func F(x Helper) {}

func main() {
    println("x")
}
