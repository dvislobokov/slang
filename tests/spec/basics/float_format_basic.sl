// test: floats print as the shortest round-trip decimal (Go %v)
// status: passing
namespace spec.basics.float_format_basic

import std.io

func main() {
    println(0.5, 2.0, 1.5e3, -0.5, 0.1 + 0.2)
    println(100.0, 123456.0, 0.0001, 3.14159)
    println(1.0 / 3.0, 2.0 / 3.0)
    var x float = 0
    println(x, float(10) / 4)
}
