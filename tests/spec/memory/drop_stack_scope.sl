// test: stack values with Drop are destroyed at the end of their block, in reverse declaration order
// status: passing
namespace spec.memory.drop_stack_scope

import std.io

struct Res {
    Name string
    func Drop() {
        println("drop", this.Name)
    }
}

func Work() {
    var a = Res{Name: "a"}
    var b = Res{Name: "b"}
    {
        var inner = Res{Name: "inner"}
        println("in block", inner.Name)
    }
    println("after block", a.Name, b.Name)
}

func main() {
    Work()
    println("after Work")
}
