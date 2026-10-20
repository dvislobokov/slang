// error: cannot call method Len on value of type int
// status: passing
namespace spec.diagnostics.method_on_primitive

import std.io

func main() {
    var n = 3
    println(n.Len())
}
