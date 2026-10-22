// error: cannot slice an array that is not stored in a variable
// status: passing
namespace spec.diagnostics.slice_temporary_array

import std.io

func Arr() [3]int {
    var a [3]int
    return a
}

func main() {
    println(Arr()[0..2])
}
