// error: does not satisfy constraint Comparable
// status: passing
namespace spec.generics2.map_key_comparable_error

import std.io
import std.collections

struct Point {
    X int
}

func main() {
    var m = Map<Point, int>{}
    println(m.Len())
}
