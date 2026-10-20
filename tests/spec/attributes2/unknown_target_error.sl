// error: unknown attribute target
// status: passing
namespace spec.attributes2.unknown_target_error

import std
import std.io

@attribute(targets: [reflect.Target.Module])
struct column {
    name string
}

func main() {
    println("x")
}
