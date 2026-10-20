// test: fixed-size arrays are values; slices share their buffer
// status: passing
namespace spec.structs.arrays_are_values

import std.io

struct Grid {
    Cells [3]int
    Tags  []string
}

func main() {
    var a [3]int
    a[0] = 1
    var b = a
    b[0] = 2
    println(a, b)
    var g = Grid{Tags: ["x", "y"]}
    var h = g
    h.Cells[1] = 9
    h.Tags[0] = "changed"
    println(g.Cells, h.Cells, g.Tags, h.Tags)
}
