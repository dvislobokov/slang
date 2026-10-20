// test: _ in a variant pattern ignores that field
// status: passing
namespace spec.enums.pattern_wildcard

import std.io

enum Shape {
    Rect(w int, h int)
    Circle(r int)
}

func Width(s Shape) int {
    switch s {
        case Shape.Rect(w, _):
            return w
        case Shape.Circle(_):
            return -1
    }
    return 0
}

func main() {
    println(Width(Shape.Rect(3, 4)), Width(Shape.Circle(9)))
}
