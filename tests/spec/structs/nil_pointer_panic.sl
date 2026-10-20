// test: field access through a nil pointer panics (exit code 2), no UB
// status: passing
// exit: 2
// stderr: nil
namespace spec.structs.nil_pointer_panic

import std.io

struct Node {
    V int
    Next *Node
}

func main() {
    var n = new Node{V: 1}
    println("first", n.V)
    var next = n.Next
    println("next is nil:", next == nil)
    println(next.V)
    println("unreachable")
}
