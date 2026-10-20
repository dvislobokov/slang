// test: any struct with the right methods satisfies an interface, no implements clause
// status: passing
namespace spec.interfaces.structural_satisfaction

import std.io

interface Shape {
    func Area() float
    func Name() string
}

struct Rect {
    W float
    H float
    func Area() float { return this.W * this.H }
    func Name() string { return "rect" }
}

struct Circle {
    R float
    func Area() float { return 3 * this.R * this.R }
    func Name() string { return "circle" }
}

func Describe(s Shape) string {
    return s.Name()
}

func main() {
    var shapes []Shape = [Rect{W: 2, H: 3}, Circle{R: 1}]
    var total = 0.0
    for s in shapes {
        println(Describe(s), s.Area())
        total += s.Area()
    }
    println("total", total)
}
