// test: Variants lists enum variants in order with their names and field counts
// status: passing
namespace spec.reflection.enum_variants

import std
import std.io

enum Shape {
    Circle(r float)
    Rect(w float, h float)
    Empty
}

func main() {
    for v in reflect.TypeOf<Shape>().Variants {
        println(v.Name, len(v.Fields))
    }
}
