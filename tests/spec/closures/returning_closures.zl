// test: functions returning closures; composition and partial application
// status: passing
namespace spec.closures.returning_closures

import std.io

func Compose(f func(int) int, g func(int) int) func(int) int {
    return (x int) => g(f(x))
}

func Adder(n int) func(int) int {
    return (x int) => x + n
}

func main() {
    var inc = Adder(1)
    var dbl = (x int) => x * 2
    println(Compose(inc, dbl)(5), Compose(dbl, inc)(5), Adder(10)(Adder(5)(0)))
}
