// error: is not generic
// status: passing
namespace spec.diagnostics.type_args_on_plain_type

import std.io

struct P { X int }

func main() {
    var p P<int>
    println(p.X)
}
