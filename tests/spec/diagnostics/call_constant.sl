// error: is a constant, not a function
// status: passing
namespace spec.diagnostics.call_constant

import std
import std.io

func main() {
    println(math.Pi(1))
}
