// error: variant pattern arguments must be names to bind
// status: passing
namespace spec.diagnostics.variant_pattern_not_names

import std.io

enum Shape {
    Circle(r int)
}

func main() {
    var sh = Shape.Circle(1)
    switch sh {
        case Shape.Circle(1): println("unit")
        default:
            println("other")
    }
}
