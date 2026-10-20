// error: array length must be an integer constant expression
// status: passing
namespace spec.diagnostics.array_length_not_constant

import std.io

func main() {
    var n = 3
    var a [n]int
    println(len(a))
}
