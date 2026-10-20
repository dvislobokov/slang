// error: type Point does not satisfy constraint Ordered
// status: pending
namespace spec.generics2.constraint_ordered_wording

import std.io

struct Point {
    X int
}

func Max<T: Ordered>(a, b T) T {
    if a > b { return a }
    return b
}

func main() {
    var p = Max(Point{X: 1}, Point{X: 2})
    println(p.X)
}
