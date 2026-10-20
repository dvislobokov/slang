// test: std.collections.Set<T>: Add (idempotent), Has, Remove, Len, Items in insertion order
// status: pending
namespace spec.generics.collections_set

import std.io
import std.collections

func main() {
    var s = Set<string>{}
    for w in ["b", "a", "b", "c", "a"] {
        s.Add(w)
    }
    println(s.Len(), s.Has("a"), s.Has("z"), s.Items())
    s.Remove("b")
    println(s.Len(), s.Items())
}
