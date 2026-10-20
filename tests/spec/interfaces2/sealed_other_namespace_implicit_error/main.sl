// error: can only be implemented in namespace
// status: passing
namespace spec.interfaces2.sealed_other_namespace_implicit_error

import std.io
import spec.interfaces2.sealed_other_namespace_implicit_error.shapes

struct Hexagon {
    func Area() float { return 6.0 }
}

func main() {
    var s shapes.Shape = Hexagon{}
    println(s.Area())
}
