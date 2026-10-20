// error: is not repeatable
// status: passing
namespace spec.attributes.repeatable_violation_error

import std.io

@attribute
struct owner {
    name string
}

@owner("a")
@owner("b")
func main() {
    println("x")
}
