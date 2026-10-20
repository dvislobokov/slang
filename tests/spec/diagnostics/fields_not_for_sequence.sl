// error: can only be the sequence of a for loop
// status: passing
namespace spec.diagnostics.fields_not_for_sequence

import std.io
import std

struct P { X int }

func Show<T>(x T) {
    var fs = reflect.Fields<T>()
    println(len(fs))
}

func main() {
    Show(P{X: 1})
}
