// error: is a type, not a value
// status: passing
namespace spec.diagnostics.generic_type_as_value

import std.io

struct Box<T> {
    V T
}

func main() {
    var b = Box<int>
    println(b)
}
