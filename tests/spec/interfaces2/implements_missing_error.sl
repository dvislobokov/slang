// error: Square does not implement Shape: missing method Perimeter
// status: pending
namespace spec.interfaces2.implements_missing_error

import std.io

interface Shape {
    func Area() float
    func Perimeter() float
}

struct Square implements Shape {
    S float
    func Area() float { return this.S * this.S }
}

func main() {
    var s = Square{S: 3}
    println(s.Area())
}
