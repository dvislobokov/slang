// error: embeds itself
// status: passing
namespace spec.interfaces2.embedding_self_error

import std.io

interface A {
    A
    func F()
}

func main() {
    var a A
    println(a == nil)
}
