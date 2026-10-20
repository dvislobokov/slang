// test: println of an any prints its dynamic value (like Go)
// status: pending
namespace spec.interfaces.any_println

import std.io

func main() {
    var a any = 42
    var b any = "text"
    var c any = 1.5
    var d any = [1, 2]
    println(a, b, c, d)
}
