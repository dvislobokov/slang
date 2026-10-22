// test: the zero value of an interface is nil and compares equal to nil
// status: passing
namespace spec.interfaces.nil_interface

import std.io

interface Named {
    func Name() string
}

struct T {
    func Name() string { return "t" }
}

func Pick(b bool) Named {
    if b {
        return T{}
    }
    return nil
}

func main() {
    var n Named
    println(n == nil, Pick(true) == nil, Pick(false) == nil)
    n = T{}
    println(n != nil, n.Name())
}
