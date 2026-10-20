// error: cannot keep an interior pointer
// status: passing
namespace spec.memory2.interior_pointer_closure_error

import std.io

func main() {
    var xs = [1, 2, 3]
    var f = func() int {
        var p = &xs[0]
        return *p
    }
    println(f())
}
