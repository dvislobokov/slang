// test: user types: Iter() returning an iterator with Next() (T, bool), a value that is itself an iterator, Next() (K, V, bool) for two variables, Next() *T ending at nil, and the Items() fallback
// status: passing
// run-flags: --leak-check
namespace spec.iteration.protocol

import std.io

struct Countdown {
    From int
    func Iter() countIter { return countIter{n: this.From} }
}

struct countIter {
    n int
    func Next() (int, bool) {
        if this.n <= 0 {
            return (0, false)
        }
        this.n -= 1
        return (this.n + 1, true)
    }
}

// a Fibonacci iterator (an iterator is iterable itself)
struct Fib {
    a     int
    b     int
    limit int
    func Next() (int, bool) {
        if this.a > this.limit {
            return (0, false)
        }
        var v = this.a
        this.a, this.b = this.b, this.a + this.b
        return (v, true)
    }
}

struct Pairs {
    keys []string
    i    int
    func Next() (string, int, bool) {
        if this.i >= len(this.keys) {
            return ("", 0, false)
        }
        this.i += 1
        return (this.keys[this.i - 1], len(this.keys[this.i - 1]), true)
    }
}

struct Node {
    Val  int
    Next *Node
}

struct Chain {
    cur *Node
    func Next() *Node {
        var n = this.cur
        if n != nil {
            this.cur = n.Next
        }
        return n
    }
}

struct Bag {
    xs []string
    func Items() []string { return this.xs }
}

interface Source {
    func Next() (int, bool)
}

func main() {
    for x in (Countdown{From: 3}) {
        print(x, " ")
    }
    println()
    for x in (Fib{a: 0, b: 1, limit: 20}) {
        print(x, " ")
    }
    println()
    for i, x in (Fib{a: 0, b: 1, limit: 3}) {
        print(i, ":", x, " ")
    }
    println()
    for k, n in (Pairs{keys: ["go", "slang"]}) {
        print(k, "=", n, " ")
    }
    println()
    var list = &Node{Val: 1, Next: &Node{Val: 2, Next: &Node{Val: 3}}}
    for n in (Chain{cur: list}) {
        print(n.Val, " ")
    }
    println()
    for i, s in (Bag{xs: ["p", "q"]}) {
        print(i, s, " ")
    }
    println()
    // an interface value with Next is an iterator too
    var src Source = &Fib{a: 1, b: 1, limit: 5}
    for x in src {
        print(x, " ")
    }
    println()
}
