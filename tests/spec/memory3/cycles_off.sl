// test: SLANG_CYCLES=off is plain reference counting: CollectCycles does nothing and a cycle leaks
// status: passing
// run-flags: --leak-check
// env: SLANG_CYCLES=off
// exit: 66
// stderr: leak: 2 objects
namespace spec.memory3.cycles_off

import std
import std.io

@allowCycle
struct Node {
    Other *Node
}

func main() {
    var a = new Node{}
    var b = new Node{Other: a}
    a.Other = b
    a = nil
    b = nil
    println("collected", runtime.CollectCycles(), runtime.CycleStats().Mode)
}
