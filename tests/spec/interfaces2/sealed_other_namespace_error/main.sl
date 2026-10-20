// error: can only be implemented in namespace
// status: passing
namespace spec.interfaces2.sealed_other_namespace_error

import std.io
import spec.interfaces2.sealed_other_namespace_error.shapes

struct Hexagon implements shapes.Shape {
    func Area() float { return 6.0 }
}

func main() {
    var h = Hexagon{}
    println(h.Area())
}
