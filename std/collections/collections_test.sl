// Examples for std.collections.
namespace collections_test

import std
import std.io

func ExampleList() {
    var l = collections.List<string>.New()
    l.Add("b")
    l.Add("c")
    l.Insert(0, "a")
    println(l.Len(), l.Items(), l.IndexOf("c"))
    // Output:
    // 3 [a b c] 2
}

func ExampleMap() {
    // Map keeps insertion order.
    var m = collections.Map<string, int>.New()
    m.Set("zeta", 1)
    m.Set("alpha", 2)
    m.Set("zeta", 3)
    println(m.Keys(), m.Values(), m.Has("beta"))
    // Output:
    // [zeta alpha] [3 2] false
}

func ExampleHeap() {
    var h = collections.Heap<int>.New()
    for x in [5, 1, 4, 2] {
        h.Push(x)
    }
    var out []int = []
    for h.Len() > 0 {
        out = append(out, try h.Pop() else 0)
    }
    println(out)
    // Output:
    // [1 2 4 5]
}
