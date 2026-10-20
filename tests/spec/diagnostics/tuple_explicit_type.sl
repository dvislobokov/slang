// error: tuple destructuring cannot have an explicit type
// status: passing
namespace spec.diagnostics.tuple_explicit_type

import std.io

func Two() (int, int) { return 1, 2 }

func main() {
    var (a, b) int = Two()
    println(a, b)
}
