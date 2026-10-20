// test: a sentinel error keeps its identity for errors.Is after ?, ?("ctx") and a copy-on-write of its trace in two callers
// status: passing
namespace spec.errors2.sentinel_is_after_propagation

import std.io
import std

const ErrMissing = error("missing")

func Find(k string) int! {
    return ErrMissing
}

func A() int! {
    return Find("a")?
}

func B() int! {
    return Find("b")?("in B")
}

func main() {
    try A() catch e1 {
        try B() catch e2 {
            println(errors.Is(e1, ErrMissing), errors.Is(e2, ErrMissing), e1, e2)
            println(len(errors.Frames(e1)), len(errors.Frames(e2)))
        }
    }
}
