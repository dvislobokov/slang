// error: duplicate parameter a
// status: passing
namespace spec.diagnostics.duplicate_parameter

import std.io

func F(a int, a int) int { return a }

func main() {
    println(F(1, 2))
}
