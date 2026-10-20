// test: importing a parent namespace gives child.Name access to nested namespaces
// status: passing
namespace spec.ns.parent

import std.io
import spec.ns.parent.lib

func main() {
    println(text.Shout("hi"), num.Twice(4))
}
