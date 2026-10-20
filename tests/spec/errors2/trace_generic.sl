// test: frames of a generic function name the instantiation (First<int>), like the instance names in DESIGN.md
// status: passing
namespace spec.errors2.trace_generic

import std.io
import std

func First<T>(xs []T) T! {
    if len(xs) == 0 {
        return error("empty list")
    }
    return xs[0]
}

func Head() int! {
    var e []int = []
    return First(e)?
}

func main() {
    try Head() catch err {
        println(err)
        for f in errors.Frames(err) {
            println(f.Func, f.Line)
        }
    }
}
