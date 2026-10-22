// error: only structs can be embedded
// status: passing
namespace spec.diagnostics.embed_non_struct

import std.io

interface I {
    func F()
}

struct S {
    I
    X int
}

func main() {
    println("x")
}
