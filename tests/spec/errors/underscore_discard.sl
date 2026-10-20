// test: _ = f() runs f and discards both the value and the error
// status: passing
namespace spec.errors.underscore_discard

import std.io

func Noisy(fail bool) int! {
    println("Noisy", fail)
    if fail {
        return error("boom")
    }
    return 1
}

func Act() void! {
    println("Act")
    return error("ignored")
}

func main() {
    _ = Noisy(false)
    _ = Noisy(true)
    _ = Act()
    println("still running")
}
