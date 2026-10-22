// test: for x in slice, for i, x in slice, for over a fixed array and a literal
// status: passing
namespace spec.control.for_slice_forms

import std.io

func main() {
    var names = ["ann", "bob", "cy"]
    for n in names {
        print(n + ";")
    }
    println()
    for i, n in names {
        println(i, n)
    }
    var arr [3]int
    arr[1] = 7
    var total = 0
    for v in arr {
        total += v
    }
    for x in [1, 2, 3] {
        total += x
    }
    println("total", total)
}
