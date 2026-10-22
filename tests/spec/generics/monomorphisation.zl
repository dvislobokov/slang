// test: each instantiation is compiled separately; + and zero values follow T (byte/int8 wrap, string concatenates)
// status: passing
namespace spec.generics.monomorphisation

import std.io

func Twice<T: Ordered>(x T) T {
    return x + x
}

func Describe<T: Ordered>(x T) string {
    var z T
    if x == z {
        return "zero value"
    }
    return "non-zero"
}

func main() {
    println(Twice(7), Twice(7.5), Twice("ab"))
    var b byte = 200
    println(Twice(b), Twice(int8(-100)))
    println(Describe(0), Describe(""), Describe("a"), Describe(0.5))
}
