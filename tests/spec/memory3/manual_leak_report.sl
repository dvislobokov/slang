// test: with SLANG_CYCLES=manual the leak report says how many leaked objects are in cycles the collector could free
// status: passing
// run-flags: --leak-check
// env: SLANG_CYCLES=manual
// exit: 66
// stderr: 2 of them in unreachable reference cycles the cycle collector can free
namespace spec.memory3.manual_leak_report

import std.io

@allowCycle
struct Node {
    Other *Node
}

func main() {
    var a = new Node{}
    var b = new Node{Other: a}
    a.Other = b
    println("built")
}
