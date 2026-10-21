// test: t.Attributes<A>() returns every instance of a repeatable attribute in source order
// status: passing
namespace spec.reflection.attributes_repeatable

import std
import std.io

@attribute(targets: [reflect.Target.Struct], repeatable: true)
struct tag {
    name string
}

@tag("entity")
@tag("audited")
struct Order {
    Id int
}

func main() {
    var tags = reflect.TypeOf<Order>().Attributes<tag>()
    println(len(tags))
    for t in tags {
        println(t.name)
    }
    println(reflect.TypeOf<Order>().Attribute<tag>() != nil)
}
