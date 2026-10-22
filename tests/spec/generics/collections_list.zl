// test: std.collections.List<T>: Add, Len, Get, Set, Contains, Remove, iteration via Items
// status: passing
namespace spec.generics.collections_list

import std.collections
import std.io

func main() {
    var l = List<int>{}
    l.Add(3)
    l.Add(1)
    l.Add(4)
    l.Set(1, 10)
    println(l.Len(), l.Get(0), l.Get(1), l.Contains(4), l.Contains(1))
    l.Remove(0)
    println(l.Items())
    var names = List<string>{}
    names.Add("x")
    println(names.Len(), names.Items())
}
