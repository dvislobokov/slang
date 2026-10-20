// error: cannot dereference value of type int
// status: passing
namespace spec.diagnostics.deref_non_pointer

import std.io

func main() {
    var n = 1
    println(*n)
}
