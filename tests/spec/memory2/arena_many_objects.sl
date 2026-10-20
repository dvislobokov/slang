// test: an arena allocates linked nodes with a.New<T>(); Free releases them all at once (leak-check clean)
// status: passing
// run-flags: --leak-check
namespace spec.memory2.arena_many_objects

import std.io
import std

struct Node {
    V int
    Next *Node
}

func main() {
    var a = arena.New()
    var head *Node = nil
    for i in 0..1000 {
        var n = a.New<Node>()
        n.V = i
        n.Next = head
        head = n
    }
    var sum = 0
    var p = head
    for p != nil {
        sum += p.V
        p = p.Next
    }
    println("sum", sum)
    head = nil
    p = nil
    a.Free()
    println("freed")
}
