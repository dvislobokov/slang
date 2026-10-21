// error: needs an interface or error subject
// status: passing
namespace spec.diagnostics.type_pattern_needs_interface

import std.io

struct P {
    X int
}

func main() {
    var n = 1
    switch n {
        case P p: println(p.X)
        default:
            println("other")
    }
}
