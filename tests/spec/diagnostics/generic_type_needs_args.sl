// error: needs type arguments
// status: passing
namespace spec.diagnostics.generic_type_needs_args

import std.io

struct Box<T> { V T }

func F(b Box) {}

func main() {
    println("x")
}
