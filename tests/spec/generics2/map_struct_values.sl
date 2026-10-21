// test: Map<K, V> with struct values: Get returns a copy, so mutation goes through Set; with *T values Get shares the object
// status: passing
namespace spec.generics2.map_struct_values

import std.collections
import std.io

struct Point {
    X int
    Y int
}

func main() {
    var m = Map<string, Point>{}
    m.Set("a", Point{X: 1, Y: 2})
    var p = m.Get("a")
    p.X = 100
    println(m.Get("a").X, p.X)
    p.Y = 7
    m.Set("a", p)
    println(m.Get("a").X, m.Get("a").Y)
    println(m.Get("missing").X, m.Has("missing"))

    var ptrs = Map<string, *Point>{}
    ptrs.Set("b", new Point{X: 5})
    ptrs.Get("b").X += 1
    var q = ptrs.Get("b")
    q.Y = 9
    println(ptrs.Get("b").X, ptrs.Get("b").Y)
    println(ptrs.Get("none") == nil)
}
