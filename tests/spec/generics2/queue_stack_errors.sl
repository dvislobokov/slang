// test: Queue/Stack Pop and Peek fail on an empty collection; the error can be caught and the collection reused
// status: passing
namespace spec.generics2.queue_stack_errors

import std.io
import std.collections

func Drain(q *Queue<int>) int! {
    var sum = 0
    for {
        var v = q.Pop()?
        sum += v
    }
    return sum
}

func main() {
    var s = Stack<string>{}
    println(try s.Pop() else "empty", try s.Peek() else "empty")
    s.Push("a")
    s.Push("b")
    println(try s.Peek() else "?", s.Len())
    println(try s.Pop() else "?", try s.Pop() else "?", try s.Pop() else "drained")
    var q = Queue<int>{}
    for i in 1..4 { q.Push(i) }
    try Drain(&q) catch err {
        println("drain stopped, len", q.Len())
    }
    q.Push(9)
    println(try q.Pop() else 0)
}
