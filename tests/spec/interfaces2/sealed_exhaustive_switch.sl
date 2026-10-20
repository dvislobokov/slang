// test: a type switch over a sealed interface that names every implementation needs no default and terminates the function
// status: pending
namespace spec.interfaces2.sealed_exhaustive_switch

import std.io
import std

sealed interface Shape {
    func Area() float
}

struct Circle {
    R float
    func Area() float { return 3.0 * this.R * this.R }
}

struct Square {
    S float
    func Area() float { return this.S * this.S }
}

func Name(s Shape) string {
    switch s {
        case Circle c: return "circle r=" + fmt.Format("{0}", c.R)
        case Square q: return "square s=" + fmt.Format("{0}", q.S)
    }
}

func main() {
    var shapes []Shape = [Circle{R: 1}, Square{S: 2.5}]
    for s in shapes {
        println(Name(s), s.Area())
    }
}
