// error: method Area has type func() int, want func() float
// status: pending
namespace spec.interfaces2.implements_wrong_signature_error

import std.io

interface Shape {
    func Area() float
}

struct Square implements Shape {
    S int
    func Area() int { return this.S * this.S }
}

func main() {
    var s = Square{S: 3}
    println(s.Area())
}
