// error: cannot call type
// status: passing
namespace spec.diagnostics.call_generic_struct_type

import std.io

struct Box<T> {
    V T
}

func main() {
    var b = Box<int>(1)
    println(b.V)
}
