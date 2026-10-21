// error: this pattern needs a switch subject
// status: passing
namespace spec.diagnostics.pattern_needs_subject

import std.io

func main() {
    var n = 1
    switch {
        case 1..5: println("small")
        default:
            println(n)
    }
}
