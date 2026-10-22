// error: cannot copy a value of type File: it has a destructor (Drop), so it is move-only
namespace app

import std.io

struct File {
    name string

    func Drop() {
        println("close", this.name)
    }
}

func main() {
    var a = File{name: "a"}
    var b = a
    println(b.name)
}
