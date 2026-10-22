// test: `implements` does not require the default methods of the interface
// status: passing
namespace spec.interfaces2.implements_default_not_required

import std.io

interface Greeter {
    func Name() string
    func Hello() string { return "hi " + this.Name() }
}

struct Cat implements Greeter {
    func Name() string { return "cat" }
}

func main() {
    var g Greeter = Cat{}
    println(g.Hello())
}
