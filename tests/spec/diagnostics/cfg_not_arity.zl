// error: not(...) in @cfg takes one condition
// status: passing
namespace spec.diagnostics.cfg_not_arity

import std.io

@cfg(not(os: "linux", os: "windows"))
func F() string { return "f" }

func main() {
    println("main")
}
