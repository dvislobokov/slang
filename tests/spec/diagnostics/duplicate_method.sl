// error: duplicate method Run in struct
// status: passing
namespace spec.diagnostics.duplicate_method

import std.io

struct S {
    func Run() {}
    func Run() {}
}

func main() {
    var s = S{}
    s.Run()
}
