// test: parallel assignment a, b = b, a evaluates the right side first
// status: pending
namespace spec.basics.multi_assign_swap

import std.io

func main() {
    var a = 1
    var b = 2
    a, b = b, a
    println(a, b)
    var s = [1, 2, 3]
    s[0], s[2] = s[2], s[0]
    println(s)
    var x = 1
    var y = 1
    for i in 0..5 {
        x, y = y, x + y
    }
    println(x, y)
}
