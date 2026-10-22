// test: type switch over an interface value with pointer and value cases and default
// status: passing
namespace spec.switch.type_switch_interface

import std.io

interface Shape {
    func Area() float
}

struct Sq {
    S float
    func Area() float { return this.S * this.S }
}

struct Tri {
    B float
    H float
    func Area() float { return this.B * this.H / 2 }
}

func Kind(s Shape) string {
    switch s {
        case Sq q:
            return "square"
        case *Tri t:
            return "triangle ptr"
        default:
            return "unknown"
    }
}

func main() {
    println(Kind(Sq{S: 1}), Kind(&Tri{B: 1, H: 2}), Kind(Tri{B: 1, H: 2}))
}
