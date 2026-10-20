// error: cannot be printed
// status: passing
namespace spec.diagnostics.not_printable

import std.io

struct P { X int }

func main() {
    println(P{X: 1})
}
