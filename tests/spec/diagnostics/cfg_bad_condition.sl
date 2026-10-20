// error: invalid @cfg condition
// status: passing
namespace spec.diagnostics.cfg_bad_condition

import std.io

@cfg(any(linux))
func F() string { return "f" }

func main() {
    println("main")
}
