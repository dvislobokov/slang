// error: is a variable; type arguments need a generic function or method
// status: passing
namespace spec.diagnostics.type_args_on_variable

import std.io

func main() {
    var f = (x int) => x
    println(f<int>(1))
}
