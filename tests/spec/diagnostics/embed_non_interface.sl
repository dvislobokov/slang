// error: only interfaces can be embedded in an interface
// status: passing
namespace spec.diagnostics.embed_non_interface

import std.io

struct P {
    X int
}

interface I {
    P
    func F()
}

func main() {
    println("x")
}
