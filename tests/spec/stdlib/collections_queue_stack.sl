// test: std.collections.Queue<T> is FIFO and Stack<T> is LIFO; Pop on empty fails
// status: pending
namespace spec.stdlib.collections_queue_stack

import std.io
import std.collections

func main() {
    var q = Queue<int>{}
    var s = Stack<int>{}
    for i in 1..4 {
        q.Push(i)
        s.Push(i)
    }
    println(try q.Pop() else -1, try q.Pop() else -1, q.Len())
    println(try s.Pop() else -1, try s.Pop() else -1, s.Len())
    _ = s.Pop()
    println(try s.Pop() else -1)
}
