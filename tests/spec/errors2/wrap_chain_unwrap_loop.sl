// test: errors.Wrap three times; an Unwrap loop walks the chain down to the root and then yields nil
// status: passing
namespace spec.errors2.wrap_chain_unwrap_loop

import std
import std.io

const ErrDisk = error("disk full")

func main() {
    var e1 = errors.Wrap(ErrDisk, "write block")
    var e2 = errors.Wrap(e1, "save file")
    var e3 = errors.Wrap(e2, "export")
    var cur error = e3
    var depth = 0
    for cur != nil {
        println(depth, cur)
        cur = errors.Unwrap(cur)
        depth += 1
    }
    println(errors.Is(e3, ErrDisk), errors.Is(e3, e2), errors.Is(e1, e3))
}
