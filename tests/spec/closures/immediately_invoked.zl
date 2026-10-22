// test: a lambda can be invoked immediately; arrow lambdas infer their return type
// status: passing
namespace spec.closures.immediately_invoked

import std.io

func main() {
    var v = func() int {
        var s = 0
        for i in 1..5 {
            s += i
        }
        return s
    }()
    var w = ((a int, b int) => a * b)(6, 7)
    var isEven = (n int) => n % 2 == 0
    println(v, w, isEven(3), isEven(4))
}
