// test: on return deferred calls run first (LIFO), then the function's values are dropped (last declared first)
// status: passing
namespace spec.memory2.drop_with_defer

import std.io

struct R {
    Name string
    func Drop() { println("drop", this.Name) }
}

func Run(early bool) {
    var a = R{Name: "a"}
    defer println("defer 1")
    var b = R{Name: "b"}
    defer func() { println("defer 2 sees", b.Name) }()
    if early {
        println("early return")
        return
    }
    var c = R{Name: "c"}
    println("normal return")
}

func main() {
    Run(true)
    println("--")
    Run(false)
}
