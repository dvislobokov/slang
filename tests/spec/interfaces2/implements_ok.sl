// test: `implements` checks at the declaration; the struct is used through the interface as usual
// status: pending
namespace spec.interfaces2.implements_ok

import std.io

interface Shape {
    func Area() float
}

struct Square implements Shape {
    S float
    func Area() float { return this.S * this.S }
}

func main() {
    var s Shape = Square{S: 3}
    println(s.Area())
}
