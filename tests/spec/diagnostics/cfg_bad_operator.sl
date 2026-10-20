// error: unknown @cfg operator
// status: passing
namespace spec.diagnostics.cfg_bad_operator

import std.io

@cfg(either(os: "linux", os: "macos"))
func F() string { return "f" }

func main() {
    println("main")
}
