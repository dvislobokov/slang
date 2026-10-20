// error: missing type arguments for generic type List
// status: passing
namespace spec.diagnostics.collection_static_needs_type_args

import std.io
import std.collections

func main() {
    var l = List()
    println(l.Len())
}
