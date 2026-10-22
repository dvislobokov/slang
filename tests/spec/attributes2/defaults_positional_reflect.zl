// test: attribute values read by reflection: named, positional and defaulted parameters
// status: passing
namespace spec.attributes2.defaults_positional_reflect

import std
import std.io

@attribute(targets: [reflect.Target.Field])
struct check {
    min     int
    max     int = 100
    message string = "out of range"
}

struct Form {
    @check(min: 0, max: 150)         Age   int
    @check(1)                        Size  int
    @check(2, 3, "custom message")   Level int
    @check(message: "named", min: 5) Count int
    Plain int
}

func main() {
    for f in reflect.TypeOf<Form>().Fields {
        var c = f.Attribute<check>()
        if c == nil {
            println(f.Name, "-")
            continue
        }
        println(f.Name, c.min, c.max, c.message)
    }
}
