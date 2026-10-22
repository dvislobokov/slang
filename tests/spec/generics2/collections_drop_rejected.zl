// error: a value with a destructor (Drop) cannot be stored
// status: passing
namespace spec.generics2.collections_drop_rejected

import std.collections
import std.io

struct Handle {
    Id int
    func Drop() { println("close", this.Id) }
}

func main() {
    var l = List<Handle>{}
    l.Add(Handle{Id: 1})
    println(l.Len())
}
