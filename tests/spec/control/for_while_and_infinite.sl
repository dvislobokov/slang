// test: condition-only for (while) and bare for with break
// status: passing
namespace spec.control.for_while_and_infinite

import std.io

func main() {
    var n = 27
    var steps = 0
    for n != 1 {
        if n % 2 == 0 {
            n = n / 2
        } else {
            n = 3 * n + 1
        }
        steps += 1
    }
    println("collatz steps:", steps)
    var i = 0
    for {
        i += 1
        if i * i > 50 {
            break
        }
    }
    println("first square over 50:", i)
    for false {
        println("never")
    }
}
