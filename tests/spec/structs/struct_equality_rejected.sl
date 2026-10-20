// error: operator == is not defined on
// status: passing
namespace spec.structs.struct_equality_rejected

import std.io

struct P {
    X int
}

func main() {
    var a = P{X: 1}
    var b = P{X: 1}
    println(a == b)
}
