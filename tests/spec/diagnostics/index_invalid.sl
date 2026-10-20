// error: cannot index value of type bool
// status: passing
namespace spec.diagnostics.index_invalid

import std.io

func main() {
    var b = true
    println(b[0])
}
