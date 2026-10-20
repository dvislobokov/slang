// error: targets must be a list
// status: passing
namespace spec.attributes2.targets_not_list_error

import std
import std.io

@attribute(targets: reflect.Target.Field)
struct column {
    name string
}

func main() {
    println("x")
}
