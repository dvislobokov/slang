// error: cannot use string as int for field X
// status: passing
namespace spec.diagnostics.field_mismatch_type

import std.io

struct P { X int }

func main() {
    var p = P{X: "one"}
    println(p.X)
}
