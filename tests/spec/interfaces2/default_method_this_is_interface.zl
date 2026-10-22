// test: `this` in a default method is the interface value: it can be passed on, and calls dispatch dynamically
// status: passing
namespace spec.interfaces2.default_method_this_is_interface

import std
import std.io

interface Shape {
    func Area() int
    func Kind() string
    func Describe() string { return Label(this) + " with area " + fmt.Format("{0}", this.Area()) }
}

func Label(s Shape) string {
    return "a " + s.Kind()
}

struct Square {
    Side int
    func Area() int { return this.Side * this.Side }
    func Kind() string { return "square" }
}

struct Rect {
    W int
    H int
    func Area() int { return this.W * this.H }
    func Kind() string { return "rect" }
}

func main() {
    var shapes []Shape = [Square{Side: 3}, Rect{W: 2, H: 5}]
    for s in shapes {
        println(s.Describe())
    }
}
