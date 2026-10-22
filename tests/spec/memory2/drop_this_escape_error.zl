// error: must not let `this` escape
// status: passing
namespace spec.memory2.drop_this_escape_error

import std.io

var last *R

struct R {
    Name string
    func Drop() { last = this }
}

func main() {
    var r = R{Name: "x"}
    println(r.Name)
}
