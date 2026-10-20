// error: cannot use nil as int
// status: passing
namespace spec.diagnostics.nil_not_nilable

import std.io

func main() {
    var n int = nil
    println(n)
}
