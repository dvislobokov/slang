// error: @cfg needs a condition
// status: passing
namespace spec.diagnostics.cfg_empty

import std.io

@cfg
func F() string { return "f" }

func main() {
    println("main")
}
