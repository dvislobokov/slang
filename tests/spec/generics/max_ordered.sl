// test: generic func with an Ordered constraint, type inferred from the arguments
// status: pending
namespace spec.generics.max_ordered

import std.io

func Max<T: Ordered>(a, b T) T {
    if a > b {
        return a
    }
    return b
}

func main() {
    println(Max(3, 7), Max(-1, -5))
    println(Max(2.5, 1.5), Max("apple", "pear"))
}
