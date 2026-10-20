// error: overflows
// status: passing
namespace spec.diagnostics.const_overflow

import std.io

const Big int8 = 300

func main() {
    println(Big)
}
