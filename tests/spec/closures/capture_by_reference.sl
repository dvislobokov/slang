// test: closures capture variables by reference: later writes are seen, and closure writes are visible outside
// status: passing
namespace spec.closures.capture_by_reference

import std.io

func main() {
    var x = 1
    var get = () => x
    var set = func(v int) {
        x = v
    }
    x = 2
    println(get())
    set(42)
    println(x, get())
}
