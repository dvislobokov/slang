// test: std.collections.Map<K, V>: Set, Get, Has, Remove, Len, Keys in insertion order
// status: passing
namespace spec.generics.collections_map

import std.io
import std

func main() {
    var m = collections.Map<string, int>{}
    m.Set("b", 2)
    m.Set("a", 1)
    m.Set("c", 3)
    m.Set("b", 20)
    println(m.Len(), m.Get("b"), m.Get("zz"), m.Has("a"), m.Has("zz"))
    m.Remove("a")
    println(m.Keys(), m.Len())
}
