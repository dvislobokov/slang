// error: wrong number of arguments in call to typeof
// status: passing
namespace spec.diagnostics.typeof_arg_count

import std.io
import std

func main() {
    println(typeof(1, 2).Name)
}
