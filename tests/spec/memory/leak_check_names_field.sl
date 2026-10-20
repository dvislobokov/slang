// test: the leak report names the field that closes the cycle (Type.Field), as a candidate for weak
// status: pending
// run-flags: --leak-check
// exit: 66
// stderr: Node.Other
namespace spec.memory.leak_check_names_field

import std.io

struct Node {
    Name string
    Other *Node
}

func main() {
    var a = new Node{Name: "a"}
    var b = new Node{Name: "b", Other: a}
    a.Other = b
    println("cycle built")
}
