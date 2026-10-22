// test: named functions and static methods are values of func type
// status: passing
namespace spec.closures.named_funcs_as_values

import std.io

func Square(n int) int {
    return n * n
}

func Apply(xs []int, f func(int) int) []int {
    var out []int = []
    for x in xs {
        out = append(out, f(x))
    }
    return out
}

func main() {
    var f func(int) int = Square
    println(Apply([1, 2, 3], f), Apply([1, 2], (n int) => -n))
    var ops = [Square, (n int) => n + 1]
    println(ops[0](4), ops[1](4))
}
