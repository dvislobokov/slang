// test: &local returned from a function escapes to the heap and stays valid
// status: passing
namespace spec.memory.escape_local_address

import std.io

struct Counter {
    N int
}

func NewCounter(start int) *Counter {
    var c = Counter{N: start}
    return &c
}

func main() {
    var a = NewCounter(1)
    var b = NewCounter(10)
    a.N += 1
    b.N += 1
    println(a.N, b.N)
}
