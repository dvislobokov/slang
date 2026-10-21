// test: Fields lists fields in declaration order with type names and C-like aligned offsets
// status: passing
namespace spec.reflection.fields_offsets

import std
import std.io

struct Rec {
    A int8
    B int64
    C int32
    D bool
    E string
}

func main() {
    var t = reflect.TypeOf<Rec>()
    println(len(t.Fields))
    for f in t.Fields {
        println(f.Name, f.Type.Name, f.Offset)
    }
}
