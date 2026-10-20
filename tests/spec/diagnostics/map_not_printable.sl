// error: cannot be printed
// status: passing
namespace spec.diagnostics.map_not_printable

import std.io

func main() {
    var m = map[string]int{"a": 1}
    println(m)
}
