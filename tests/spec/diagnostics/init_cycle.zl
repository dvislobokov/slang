// error: initialization cycle
// status: passing
namespace spec.diagnostics.init_cycle

import std.io

var a = b + 1
var b = a + 1

func main() {
    println(a, b)
}
