// test: a closure capturing a *T keeps the object alive until the closure itself is released
// status: passing
// run-flags: --leak-check
namespace spec.memory2.closure_captured_pointer_released

import std.io

struct Counter {
    N int
    func Drop() { println("drop counter", this.N) }
}

func MakeInc() func() int {
    var c = new Counter{}
    return func() int {
        c.N += 1
        return c.N
    }
}

func main() {
    var inc = MakeInc()
    inc()
    println("value", inc())
    inc = nil
    println("after release")
}
