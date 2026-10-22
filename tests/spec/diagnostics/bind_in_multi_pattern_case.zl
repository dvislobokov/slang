// error: cannot bind a variable in a case with several patterns
// status: passing
namespace spec.diagnostics.bind_in_multi_pattern_case

import std.io

enum Shape {
    Circle(r float)
    Square(s float)
}

func main() {
    var sh = Shape.Circle(1.0)
    switch sh {
        case Shape.Circle(x), Shape.Square(x): println(x)
    }
}
