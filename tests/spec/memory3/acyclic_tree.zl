// test: @acyclic promises that a type's values never form a cycle: no W0001, and its objects are neither flagged nor recorded as possible cycle roots
// status: passing
// run-flags: --leak-check
// no-warning: W0001
namespace spec.memory3.acyclic_tree

import std
import std.io

@acyclic
struct Tree {
    Left  *Tree
    Right *Tree
}

func build(depth int) *Tree {
    if depth == 0 {
        return new Tree{}
    }
    return new Tree{Left: build(depth - 1), Right: build(depth - 1)}
}

func count(t *Tree) int {
    if t == nil {
        return 0
    }
    return 1 + count(t.Left) + count(t.Right)
}

func main() {
    var t = build(10)
    println("nodes", count(t))
    println("candidates", runtime.CycleStats().Candidates)
}
