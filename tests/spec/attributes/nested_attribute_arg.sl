// test: attribute parameters may be enums, slices and other attribute structs
// status: passing
namespace spec.attributes.nested_attribute_arg

import std.io

enum Level { Low, High }

@attribute
struct unit {
    name string
}

@attribute
struct meta {
    level Level
    tags []string
    u unit
}

@meta(level: Level.High, tags: ["a", "b"], u: unit{name: "ms"})
func main() {
    println("ok")
}
