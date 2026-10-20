// error: cannot convert
// status: passing
namespace spec.diagnostics.bad_conversion

import std.io

struct P { X int }

func main() {
    println(int(P{X: 1}))
}
