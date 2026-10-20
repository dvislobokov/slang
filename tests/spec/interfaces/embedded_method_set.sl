// test: methods promoted from an embedded struct count toward satisfying an interface
// status: passing
namespace spec.interfaces.embedded_method_set

import std.io

interface Speaker {
    func Speak() string
    func Name() string
}

struct Animal {
    N string
    func Name() string { return this.N }
}

struct Dog {
    Animal
    func Speak() string { return this.N + ": woof" }
}

func Greet(s Speaker) {
    println(s.Name(), "->", s.Speak())
}

func main() {
    var d = Dog{Animal: Animal{N: "rex"}}
    Greet(d)
    Greet(&d)
}
