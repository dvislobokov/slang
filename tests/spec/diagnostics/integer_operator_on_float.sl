// error: requires integer operands
// status: passing
namespace spec.diagnostics.integer_operator_on_float

import std.io

func main() {
    var f = 1.5
    println(f << 1)
}
