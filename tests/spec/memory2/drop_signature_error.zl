// error: must be declared as `func Drop()`
// status: passing
namespace spec.memory2.drop_signature_error

import std.io

struct R {
    Name string
    func Drop() bool { return true }
}

func main() {
    var r = R{Name: "x"}
    println(r.Name)
}
