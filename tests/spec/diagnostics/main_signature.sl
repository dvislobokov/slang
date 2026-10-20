// error: main must take no parameters and return nothing (or void!)
// status: passing
namespace spec.diagnostics.main_signature

import std.io

func main() int {
    println("x")
    return 0
}
