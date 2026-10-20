// error: duplicate case
// status: passing
namespace spec.switch.duplicate_case_error

import std.io

func main() {
    var n = 2
    switch n {
        case 1, 2:
            println("a")
        case 2:
            println("b")
    }
}
