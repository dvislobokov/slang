// error: embeds itself
// (mutual embedding A -> B -> A)
// status: pending
namespace spec.interfaces2.embedding_cycle_error

import std.io

interface A {
    B
    func F()
}

interface B {
    A
}

func main() {
    var a A
    println(a == nil)
}
