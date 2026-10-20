// error: has both a field and a method named Size
// status: passing
namespace spec.diagnostics.field_and_method_same_name

import std.io

struct S {
    Size int
    func Size() int { return 1 }
}

func main() {
    var s = S{}
    println(s.Size)
}
