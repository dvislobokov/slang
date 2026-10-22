// error: is not generic (type arguments are only allowed on generic functions, methods and types)
// status: passing
namespace spec.diagnostics.type_args_on_plain_func

import std.io

func F(x int) int { return x }

func main() {
    println(F<int>(1))
}
