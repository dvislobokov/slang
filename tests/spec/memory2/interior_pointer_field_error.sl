// error: cannot keep an interior pointer
// status: passing
namespace spec.memory2.interior_pointer_field_error

import std.io

struct Inner {
    V int
}

struct Outer {
    In Inner
}

struct Ref {
    P *Inner
}

func main() {
    var o = new Outer{}
    var r = Ref{P: &o.In}
    println(r.P.V)
}
