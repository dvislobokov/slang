// error: cannot copy a value of type
// status: passing
namespace spec.memory.drop_copy_error

import std.io

struct Handle {
    Fd int
    func Drop() {
        println("close", this.Fd)
    }
}

func main() {
    var a = Handle{Fd: 3}
    var b = a
    println(a.Fd, b.Fd)
}
