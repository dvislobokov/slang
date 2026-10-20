// test: append(xs, a, b, c) appends every value (in place and into a new slice); all values are evaluated before the first is appended
// status: passing
// run-flags: --leak-check
namespace spec.basics.append_many

import std.io

func main() {
    var xs = [1]
    xs = append(xs, 2, 3, 4)
    println(xs, len(xs))
    var ys = append(xs, len(xs), len(xs))
    println(ys, xs)
    var ss []string
    ss = append(ss, "a", "b", "c")
    var tt = append(ss, "d", "e")
    println(ss, tt, len(tt))
    var grid [][]int
    grid = append(grid, [1], [2, 2])
    println(grid)
}
