// test: Set<string> union, intersection and difference written with Add/Has/Remove, in insertion order
// status: passing
namespace spec.generics2.set_string_ops

import std.collections
import std.io

func Of(xs []string) Set<string> {
    var s = Set<string>{}
    for x in xs { s.Add(x) }
    return s
}

func Union(a, b Set<string>) Set<string> {
    var out = Set<string>{}
    for x in a { out.Add(x) }
    for x in b { out.Add(x) }
    return out
}

func Intersect(a, b Set<string>) Set<string> {
    var out = Set<string>{}
    for x in a {
        if b.Has(x) { out.Add(x) }
    }
    return out
}

func Minus(a, b Set<string>) Set<string> {
    var out = Set<string>{}
    for x in a.Items() { out.Add(x) }
    for x in b { out.Remove(x) }
    return out
}

func main() {
    var a = Of(["pear", "apple", "fig", "apple"])
    var b = Of(["fig", "kiwi", "pear"])
    println(a.Len(), b.Len())
    println(Union(a, b).Items())
    println(Intersect(a, b).Items())
    println(Minus(a, b).Items())
    println(Minus(b, a).Items(), Minus(a, a).Len())
}
