// error: is not generic (it takes no type arguments)
// status: passing
namespace spec.diagnostics.type_args_on_plain_method

import std.io

struct S {
    func F(x int) int { return x }
}

func main() {
    var s = S{}
    println(s.F<int>(1))
}
