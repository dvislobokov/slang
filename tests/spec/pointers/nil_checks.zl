// test: nil pointers: zero value, comparison, returned nil, nil fields of new structs
// status: passing
namespace spec.pointers.nil_checks

import std.io

struct Node {
    V    int
    Next *Node
}

func Find(n *Node, v int) *Node {
    var cur = n
    for cur != nil {
        if cur.V == v {
            return cur
        }
        cur = cur.Next
    }
    return nil
}

func main() {
    var list = new Node{V: 1, Next: new Node{V: 2}}
    var p *Node
    println(p == nil, list.Next.Next == nil, Find(list, 2) != nil, Find(list, 3) == nil)
    println(Find(list, 2).V)
}
