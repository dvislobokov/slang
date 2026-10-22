// test: (a, b) = f() assigns a tuple result to existing variables
// status: passing
namespace spec.basics.tuple_assign

import std.io

func DivMod(a, b int) (int, int) {
    return a / b, a % b
}

func main() {
    var q = 0
    var r = 0
    (q, r) = DivMod(17, 5)
    println(q, r)
    (q, _) = DivMod(9, 2)
    println(q, r)
}
