// error: is a namespace, not a value
// status: passing
namespace spec.diagnostics.namespace_as_value

import std.io
import std

func main() {
    var x = std
    println(x)
}
