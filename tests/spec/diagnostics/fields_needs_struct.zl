// error: reflect.Fields<T>() needs a struct type
// status: passing
namespace spec.diagnostics.fields_needs_struct

import std
import std.io

func Show<T>(x T) {
    for f in reflect.Fields<T>() {
        println(f.Name)
    }
}

func main() {
    Show(1)
}
