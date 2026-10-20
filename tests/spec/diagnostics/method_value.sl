// error: method values are not supported
// status: passing
namespace spec.diagnostics.method_value

import std.io

struct S {
    func F() int { return 1 }
}

func main() {
    var s = S{}
    var f = s.F
    println(f())
}
