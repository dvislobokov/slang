// error: operator + is not defined on type parameter T
// status: passing
namespace spec.generics2.unconstrained_operator_error

import std.io

func Sum<T>(xs []T) T {
    var t T
    for x in xs { t = t + x }
    return t
}

func main() {
    println(Sum([1, 2]))
}
