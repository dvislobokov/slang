// test: a custom attribute struct with defaults, used with named and positional arguments
// status: passing
namespace spec.attributes.custom_attribute

import std
import std.io

@attribute(targets: [reflect.Target.Field])
struct range {
    min     int
    max     int
    message string = "value out of range"
}

struct User {
    @range(min: 0, max: 150) Age   int
    @range(1, 64)            Name  string
    @range(1, 2, "custom")   Level int
}

func main() {
    var u = User{Age: 30, Name: "ann", Level: 1}
    println(u.Age, u.Name, u.Level)
}
