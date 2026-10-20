// error: cannot assign
// status: passing
namespace spec.diagnostics.sealed_from_open_interface

import std.io

sealed interface Shape {
    func Area() int
}

interface Sized {
    func Area() int
}

struct Sq {
    func Area() int { return 4 }
}

func main() {
    var s Sized = Sq{}
    var sh Shape = s
    println(sh.Area())
}
