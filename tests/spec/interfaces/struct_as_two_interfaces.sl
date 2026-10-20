// test: one struct satisfies several interfaces; dynamic dispatch picks the right method
// status: passing
namespace spec.interfaces.struct_as_two_interfaces

import std.io

interface Named {
    func Name() string
}

interface Sized {
    func Size() int
}

struct File {
    Path string
    Bytes int
    func Name() string { return this.Path }
    func Size() int { return this.Bytes }
}

struct Dir {
    func Name() string { return "dir" }
}

func main() {
    var f = File{Path: "a.txt", Bytes: 12}
    var names []Named = [f, Dir{}]
    var sizes []Sized = [f]
    for n in names {
        println(n.Name())
    }
    println(sizes[0].Size())
}
