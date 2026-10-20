// test: an interface method with a body is a default: implementations only provide the methods without a body
// status: passing
namespace spec.interfaces2.default_method_call

import std.io

interface Named {
    func Name() string
    func Greet() string { return "hello, " + this.Name() }
}

struct Bob {
    func Name() string { return "Bob" }
}

struct Alice {
    func Name() string { return "Alice" }
}

func main() {
    var people []Named = [Bob{}, Alice{}]
    for p in people {
        println(p.Greet())
    }
}
