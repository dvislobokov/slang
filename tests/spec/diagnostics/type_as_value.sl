// error: is a type, not a value
// status: passing
namespace spec.diagnostics.type_as_value

import std.io

struct P { X int }

func main() {
    var x = P
    println(x)
}
