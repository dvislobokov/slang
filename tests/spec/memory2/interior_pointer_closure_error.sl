// error: cannot be captured by a closure
// status: passing
namespace spec.memory2.interior_pointer_closure_error

import std.io

func main() {
    var xs = [1, 2, 3]
    var p = &xs[0]
    var f = func() int {
        return *p
    }
    println(f())
}
