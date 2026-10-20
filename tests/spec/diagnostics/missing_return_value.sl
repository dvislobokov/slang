// error: missing return value: the function returns int
// status: passing
namespace spec.diagnostics.missing_return_value

import std.io

func F() int {
    return
}

func main() {
    println(F())
}
