// error: cannot compare int with nil
// status: passing
namespace spec.diagnostics.compare_with_nil_invalid

import std.io

func main() {
    var n = 1
    println(n == nil)
}
