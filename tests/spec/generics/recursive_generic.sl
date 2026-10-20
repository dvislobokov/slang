// test: a generic function calling itself recursively
// status: pending
namespace spec.generics.recursive_generic

import std.io

func Reverse<T>(xs []T) []T {
    if len(xs) <= 1 {
        return xs
    }
    var rest = Reverse(xs[1..len(xs)])
    return append(rest, xs[0])
}

func Depth<T>(x T, n int) int {
    if n == 0 {
        return 0
    }
    return 1 + Depth(x, n - 1)
}

func main() {
    println(Reverse([1, 2, 3, 4]), Reverse(["a", "b"]))
    println(Depth("s", 5))
}
