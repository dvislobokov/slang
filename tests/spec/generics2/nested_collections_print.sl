// test: std.collections List<List<int>> (declared with a trailing >>); a List stored in a Map is a value copy (its Len does not follow later Adds)
// status: passing
namespace spec.generics2.nested_collections_print

import std.collections
import std.io

func main() {
    var grid List<List<int>>
    for r in 0..3 {
        var row = List<int>{}
        for c in 0..r + 1 {
            row.Add(r * 10 + c)
        }
        grid.Add(row)
    }
    println(grid.Len())
    for row in grid {
        println(row.Items())
    }
    var m = Map<string, List<string>>{}
    var l = List<string>{}
    l.Add("x")
    m.Set("k", l)
    l.Add("y")
    println(m.Get("k").Len(), l.Len())
}
