// test: attributes on a generic struct and its fields are shared by every instantiation
// status: pending
namespace spec.attributes2.on_generic_struct

import std.io
import std

@attribute
struct note {
    text string
}

@note("container")
struct Box<T> {
    @note("payload") V T
    Count int
}

func main() {
    var ti = reflect.TypeOf<Box<int>>()
    var ts = reflect.TypeOf<Box<string>>()
    println(ti.Attribute<note>().text, ts.Attribute<note>().text)
    for f in ts.Fields {
        var n = f.Attribute<note>()
        if n != nil {
            println(f.Name, f.Type.Name, n.text)
        } else {
            println(f.Name, f.Type.Name, "-")
        }
    }
}
