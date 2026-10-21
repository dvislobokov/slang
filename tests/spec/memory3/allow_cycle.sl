// test: @allowCycle silences W0001 for the type (the collector still frees its cycles)
// status: passing
// no-warning: W0001
namespace spec.memory3.allow_cycle

import std
import std.io

@allowCycle
struct List {
    Value int
    Next  *List
}

func main() {
    var a = new List{Value: 1}
    a.Next = a
    a = nil
    println("collected", runtime.CollectCycles())
}
