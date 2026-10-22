// error: f.Get needs a value of type
// status: passing
namespace spec.diagnostics.field_get_wrong_type

import std
import std.io

struct P {
    X int
}

func Show<T>(x T) {
    for f in reflect.Fields<T>() {
        println(f.Get(1))
    }
}

func main() {
    Show(P{X: 1})
}
