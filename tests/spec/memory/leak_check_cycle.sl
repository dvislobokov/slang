// test: slang run --leak-check reports objects kept alive by a strong cycle on stderr and exits with 66 when the cycle collector does not run (SLANG_CYCLES=manual)
// status: passing
// run-flags: --leak-check
// env: SLANG_CYCLES=manual
// exit: 66
// stderr: leak
namespace spec.memory.leak_check_cycle

import std.io

struct Node {
    Name  string
    Other *Node
}

func main() {
    var a = new Node{Name: "a"}
    var b = new Node{Name: "b", Other: a}
    a.Other = b
    println("cycle built")
}
