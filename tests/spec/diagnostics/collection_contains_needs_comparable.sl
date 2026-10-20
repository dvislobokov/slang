// error: (required by List<
// status: passing
namespace spec.diagnostics.collection_contains_needs_comparable

import std.io
import std.collections

struct P { X int }

func main() {
    var l = List<P>{}
    l.Add(P{X: 1})
    println(l.Contains(P{X: 1}))
}
