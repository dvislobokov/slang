// error: is not an interface
// status: pending
namespace spec.interfaces2.implements_not_interface_error

import std.io

struct Base {
    X int
}

struct Derived implements Base {
    Y int
}

func main() {
    var d = Derived{Y: 1}
    println(d.Y)
}
