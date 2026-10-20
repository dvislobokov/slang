// error: cannot be applied to a field
// status: passing
namespace spec.attributes2.targets_variant_violation_error

import std
import std.io

@attribute(targets: [reflect.Target.Variant])
struct label {
    text string
}

struct Task {
    @label("x") Name string
}

func main() {
    println(Task{Name: "a"}.Name)
}
