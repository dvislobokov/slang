// error: type Sq does not satisfy constraint Shape: missing method Area
// status: pending
namespace spec.generics2.constraint_interface_wording

import std.io

interface Shape {
    func Area() float
}

struct Sq {
    S float
}

func Total<T: Shape>(xs []T) float {
    var t = 0.0
    for x in xs { t += x.Area() }
    return t
}

func main() {
    println(Total([Sq{S: 1}]))
}
