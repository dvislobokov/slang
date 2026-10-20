// test: in-place linked list reversal by relinking pointers
// status: passing
namespace spec.pointers.linked_list_reverse

import std.io

struct Node {
    V    int
    Next *Node
}

func Reverse(head *Node) *Node {
    var prev *Node
    var cur = head
    for cur != nil {
        var next = cur.Next
        cur.Next = prev
        prev = cur
        cur = next
    }
    return prev
}

func main() {
    var head *Node
    for i in 0..5 {
        head = new Node{V: i, Next: head}
    }
    head = Reverse(head)
    var out []int = []
    var n = head
    for n != nil {
        out = append(out, n.V)
        n = n.Next
    }
    println(out)
}
