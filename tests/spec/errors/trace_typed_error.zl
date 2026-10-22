// test: errors.Trace for a struct error returned directly and propagated once
// status: passing
namespace spec.errors.trace_typed_error

import std
import std.io

struct Timeout {
    Ms int
    func Message() string { return fmt.Format("timeout after {0}ms", this.Ms) }
}

func Dial(ms int) int! {
    if ms > 10 {
        return Timeout{Ms: ms}
    }
    return ms
}

func Connect() int! {
    var c = Dial(50)?
    return c
}

func main() {
    var c = try Connect() catch err {
        println(err)
        println(errors.Trace(err))
        return
    }
    println(c)
}
