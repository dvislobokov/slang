// error: missing argument max
// status: passing
namespace spec.attributes.missing_arg_error

import std.io

@attribute
struct limit {
    min int
    max int
}

@limit(min: 1)
func main() {
    println("x")
}
