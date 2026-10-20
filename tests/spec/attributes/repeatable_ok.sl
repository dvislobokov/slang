// test: a repeatable attribute may appear several times on one declaration
// status: passing
namespace spec.attributes.repeatable_ok

import std
import std.io

@attribute(targets: [reflect.Target.Struct, reflect.Target.Func], repeatable: true)
struct tag {
    name string
}

@tag("entity")
@tag("audited")
struct Order {
    Id int
}

@tag("helper")
func Make() Order {
    return Order{Id: 1}
}

func main() {
    println(Make().Id)
}
