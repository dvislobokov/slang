// error: has no method Close
// status: passing
namespace spec.diagnostics.interface_no_method

import std.io

interface Reader {
    func Read() string
}

func Use(r Reader) {
    r.Close()
}

func main() {
    println("x")
}
