// error: try requires a fallible (T!) expression
// status: passing
namespace spec.diagnostics.try_non_fallible

import std.io

func F() int { return 1 }

func main() {
    var n = try F() else 0
    println(n)
}
