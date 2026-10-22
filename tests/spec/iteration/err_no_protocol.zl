// error: cannot iterate over value of type Point
// status: passing
namespace spec.iteration.err_no_protocol

import std.io

struct Point {
    X int
}

func main() {
    for x in (Point{X: 1}) {
        println(x)
    }
}
