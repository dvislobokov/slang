// test: closures inside generic functions capture values of type T and keep them per instantiation
// status: passing
namespace spec.generics2.closure_captures_t

import std.io

func Accumulator<T: Ordered>(start T) func(T) T {
    var total = start
    return func(x T) T {
        total = total + x
        return total
    }
}

func Constant<T>(v T) func() T {
    return () => v
}

func main() {
    var ai = Accumulator(10)
    ai(5)
    println(ai(1))
    var acc = Accumulator("a")
    acc("b")
    println(acc("c"))
    var af = Accumulator(0.5)
    println(af(0.25))
    var k = Constant([1, 2])
    println(k(), len(k()))
}
