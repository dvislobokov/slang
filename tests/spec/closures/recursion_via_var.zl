// test: a closure stored in a declared variable can call itself through that variable
// status: passing
namespace spec.closures.recursion_via_var

import std.io

func main() {
    var fib func(int) int
    fib = func(n int) int {
        if n < 2 {
            return n
        }
        return fib(n - 1) + fib(n - 2)
    }
    println(fib(10), fib(20))
}
