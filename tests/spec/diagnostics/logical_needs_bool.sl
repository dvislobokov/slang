// error: requires bool operands
// status: passing
namespace spec.diagnostics.logical_needs_bool

import std.io

func main() {
    var a = 1
    println(a && true)
}
