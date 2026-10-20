// error: unknown identifier _
// status: passing
namespace spec.basics.underscore_not_readable

import std.io

func main() {
    var x = _
    println(x)
}
