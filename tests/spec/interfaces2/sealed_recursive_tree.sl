// test: an exhaustive switch over a sealed interface works over a recursive tree of implementations
// status: pending
namespace spec.interfaces2.sealed_recursive_tree

import std.io

sealed interface Node {
    func Weight() int
}

struct Leaf {
    V int
    func Weight() int { return 1 }
}

struct Pair {
    L Node
    R Node
    func Weight() int { return this.L.Weight() + this.R.Weight() }
}

func Sum(n Node) int {
    switch n {
        case Leaf l: return l.V
        case Pair p: return Sum(p.L) + Sum(p.R)
    }
}

func main() {
    var t Node = Pair{L: Leaf{V: 3}, R: Pair{L: Leaf{V: 4}, R: Leaf{V: 5}}}
    println(Sum(t), t.Weight())
}
