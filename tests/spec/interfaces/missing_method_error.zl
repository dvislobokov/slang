// error: missing method Perimeter, missing method Name
// status: passing
namespace spec.interfaces.missing_method_error

import std.io

interface Shape {
    func Area() float
    func Perimeter() float
    func Name() string
}

struct Square {
    S float
    func Area() float { return this.S * this.S }
}

func Show(s Shape) {
    println(s.Area())
}

func main() {
    Show(Square{S: 2})
}
