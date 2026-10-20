// error: has no field
// status: passing
namespace spec.diagnostics.field_on_non_struct

import std.io

func main() {
    var n = 3
    println(n.Size)
}
