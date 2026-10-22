// error: in instantiation of Show<Point>
// status: passing
namespace spec.generics2.instantiation_note

import std.io

struct Point {
    X int
}

func Show<T>(x T) {
    println(x)
}

func main() {
    Show(1)
    Show(Point{X: 1})
}
