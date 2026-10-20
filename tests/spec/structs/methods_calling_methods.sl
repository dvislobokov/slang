// test: methods call other methods through this; recursive methods work
// status: passing
namespace spec.structs.methods_calling_methods

import std.io

struct Tree {
    V int
    Left *Tree
    Right *Tree

    func Sum() int {
        var s = this.V
        if this.Left != nil {
            s += this.Left.Sum()
        }
        if this.Right != nil {
            s += this.Right.Sum()
        }
        return s
    }

    func Double() int {
        return this.Sum() * 2
    }
}

func main() {
    var t = Tree{V: 1, Left: new Tree{V: 2}, Right: new Tree{V: 3, Left: new Tree{V: 4}}}
    println(t.Sum(), t.Double())
}
