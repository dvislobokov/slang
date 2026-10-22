// error: needs type arguments to be used as a value
// status: passing
namespace spec.diagnostics.generic_needs_type_args_value

import std.io

func Id<T>(x T) T { return x }

func main() {
    var f = Id
    println(f(1))
}
