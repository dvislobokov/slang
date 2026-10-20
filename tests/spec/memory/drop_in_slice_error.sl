// error: cannot be stored as a slice
// status: passing
namespace spec.memory.drop_in_slice_error

import std.io

struct Handle {
    Fd int
    func Drop() {
        println("close", this.Fd)
    }
}

func main() {
    var hs = [Handle{Fd: 1}]
    println(len(hs))
}
