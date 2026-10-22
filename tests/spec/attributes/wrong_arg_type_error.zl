// error: attribute argument has type string, want int
// status: passing
namespace spec.attributes.wrong_arg_type_error

import std.io

@attribute
struct limit {
    max int
}

@limit(max: "ten")
func main() {
    println("x")
}
