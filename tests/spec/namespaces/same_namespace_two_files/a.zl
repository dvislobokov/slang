// test: two files may declare the same namespace and see each other's declarations
// status: passing
namespace spec.ns.shared

import std.io

func main() {
    var c = Counter{}
    c.Add(Step)
    c.Add(Step)
    println(c.N, describe(c))
}
