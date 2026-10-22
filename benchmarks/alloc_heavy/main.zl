namespace bench.allocheavy

import std
import std.io

// Two phases: binary trees (benchmarks game shape: many short-lived small
// heap nodes), then 4M rounds of small temporaries whose address is taken
// but does not escape (`new Stats{}`, `&acc` passed to a method), which
// escape analysis keeps on the stack (Go's does too).

struct Node {
    Left  *Node
    Right *Node
}

struct Stats {
    Nodes int
    Trees int

    func Add(n int) {
        this.Nodes += n
        this.Trees += 1
    }
}

func build(depth int) *Node {
    if depth == 0 {
        return new Node{}
    }
    return new Node{Left: build(depth - 1), Right: build(depth - 1)}
}

func check(n *Node) int {
    if n.Left == nil {
        return 1
    }
    return 1 + check(n.Left) + check(n.Right)
}

func main() {
    var maxDepth = 16
    var long = build(maxDepth)
    var total = 0
    var depth = 4
    for depth <= maxDepth {
        var iters = 1 << (maxDepth - depth + 4)
        var acc = Stats{}
        for _ in 0..iters {
            var st = new Stats{}
            st.Add(check(build(depth)))
            acc.Add(st.Nodes)
        }
        total += acc.Nodes + acc.Trees
        depth += 2
    }
    var temps = 0
    for i in 0..4000000 {
        var st = new Stats{}
        st.Add(i & 7)
        var acc = Stats{Nodes: st.Nodes}
        acc.Add(1)
        temps += acc.Nodes + acc.Trees + st.Trees
    }
    println(total, check(long), temps)
}
