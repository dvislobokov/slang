// error: cannot call type
// status: passing
namespace spec.diagnostics.call_struct_type

import std.io

struct Point { X int }

func main() {
    var p = Point(1)
    println(p.X)
}
