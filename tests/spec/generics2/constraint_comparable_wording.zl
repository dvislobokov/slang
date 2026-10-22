// error: does not satisfy constraint Comparable
// status: passing
namespace spec.generics2.constraint_comparable_wording

import std.io

struct Point {
    X int
}

func IndexOf<T: Comparable>(xs []T, x T) int {
    for i, v in xs {
        if v == x { return i }
    }
    return -1
}

func main() {
    println(IndexOf([Point{X: 1}], Point{X: 1}))
}
