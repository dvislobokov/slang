// error: non-boolean condition
// status: passing
namespace spec.control.if_condition_not_bool

import std.io

func main() {
    var n = 1
    if n {
        println("go would reject this")
    }
}
