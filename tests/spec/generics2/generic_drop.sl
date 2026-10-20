// test: a generic struct with a destructor: Drop runs for every instantiation, in reverse declaration order
// status: passing
namespace spec.generics2.generic_drop

import std.io

struct Guard<T> {
    V T
    func Drop() { println("drop", this.V) }
}

func main() {
    var a = Guard<int>{V: 1}
    var b = Guard<string>{V: "two"}
    {
        var c = Guard<float>{V: 3.5}
        println("inner")
    }
    println("outer", a.V, b.V)
}
