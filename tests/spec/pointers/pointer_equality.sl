// test: pointers are equal exactly when they point to the same object
// status: passing
namespace spec.pointers.pointer_equality

import std.io

struct P {
    X int
}

func main() {
    var a = P{X: 1}
    var b = P{X: 1}
    var pa = &a
    var pa2 = &a
    var pb = &b
    var h1 = new P{X: 1}
    var h2 = h1
    println(pa == pa2, pa == pb, pa != pb, h1 == h2, h1 == new P{X: 1})
}
