// test: a struct that declares a method with the name of a default method overrides the default
// status: passing
namespace spec.interfaces2.default_method_override

import std.io

interface Named {
    func Name() string
    func Greet() string { return "hello, " + this.Name() }
}

struct Plain {
    func Name() string { return "plain" }
}

struct Polite {
    func Name() string { return "polite" }
    func Greet() string { return "good evening, " + this.Name() }
}

func Show(n Named) {
    println(n.Greet())
}

func main() {
    Show(Plain{})
    Show(Polite{})
    var p = Polite{}
    println(p.Greet())
}
