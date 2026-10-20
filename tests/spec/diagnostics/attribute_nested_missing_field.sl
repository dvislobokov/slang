// error: missing field name in attribute value unit{...}
// status: passing
namespace spec.diagnostics.attribute_nested_missing_field

import std.io

@attribute
struct unit {
    name string
}

@attribute
struct metric {
    u unit
}

@metric(u: unit{})
struct A { X int }

func main() {
    println(A{X: 1}.X)
}
