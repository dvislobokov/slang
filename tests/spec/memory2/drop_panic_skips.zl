// test: a panic does not unwind: destructors of live values do not run, exit code 2
// status: passing
// exit: 2
// stderr: boom
namespace spec.memory2.drop_panic_skips

import std.io

struct R {
    Name string
    func Drop() { println("drop", this.Name) }
}

func main() {
    var a = R{Name: "a"}
    {
        var b = R{Name: "b"}
    }
    println("before panic")
    panic("boom")
}
