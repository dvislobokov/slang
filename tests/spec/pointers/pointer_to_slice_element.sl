// test: a pointer to a slice element can be kept in a variable (Go semantics); writes are visible in the slice
// status: pending
namespace spec.pointers.pointer_to_slice_element

import std.io

func main() {
    var xs = [1, 2, 3]
    var p = &xs[1]
    *p = 20
    println(xs, *p)
}
