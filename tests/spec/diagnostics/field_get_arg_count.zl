// error: f.Get takes one argument
// status: passing
namespace spec.diagnostics.field_get_arg_count

import std
import std.io

struct P {
    X int
}

func Show<T>(x T) {
    for f in reflect.Fields<T>() {
        println(f.Get())
    }
}

func main() {
    Show(P{X: 1})
}
