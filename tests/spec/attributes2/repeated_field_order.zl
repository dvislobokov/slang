// test: Attributes<A>() of a field returns every instance of a repeatable attribute in source order
// status: passing
namespace spec.attributes2.repeated_field_order

import std
import std.io

@attribute(repeatable: true)
struct rule {
    name string
}

struct Signup {
    @rule("required") @rule("email") @rule("max:64") Email string
    @rule("optional")                                Nick  string
}

func main() {
    for f in reflect.TypeOf<Signup>().Fields {
        var rs = f.Attributes<rule>()
        var names []string = []
        for r in rs {
            names = append(names, r.name)
        }
        println(f.Name, len(rs), names)
    }
}
