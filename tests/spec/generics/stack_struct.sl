// test: generic struct Stack<T> with methods, instantiated twice
// status: pending
namespace spec.generics.stack_struct

import std.io

struct Stack<T> {
    items []T

    func Push(x T) {
        this.items = append(this.items, x)
    }

    func Pop() T! {
        if len(this.items) == 0 {
            return error("empty stack")
        }
        var x = this.items[len(this.items) - 1]
        this.items = this.items[0..len(this.items) - 1]
        return x
    }

    func Len() int {
        return len(this.items)
    }
}

func main() {
    var s = Stack<int>{}
    s.Push(1)
    s.Push(2)
    s.Push(3)
    println(s.Len(), try s.Pop() else -1, try s.Pop() else -1, s.Len())
    var w = Stack<string>{}
    w.Push("only")
    println(try w.Pop() else "?", try w.Pop() else "empty")
}
