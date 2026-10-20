// error: not exhaustive
// status: pending
namespace spec.interfaces2.sealed_not_exhaustive_error

import std.io

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

struct Triangle {
    B float
    H float
    func Area() float { return this.B * this.H / 2.0 }
}

func Describe(s Shape) {
    switch s {
        case Circle c: println("circle", c.R)
        case Square q: println("square", q.S)
    }
}

func main() {
    Describe(Triangle{B: 1, H: 2})
}
