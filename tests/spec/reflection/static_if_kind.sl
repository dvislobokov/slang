// test: an if on reflect.TypeOf<T>().Kind is resolved at compile time; only the taken branch is checked
// status: passing
namespace spec.reflection.static_if_kind

import std.io
import std

func Twice<T>(x T) T {
    if reflect.TypeOf<T>().Kind == reflect.Kind.String {
        return x + x
    } else if reflect.TypeOf<T>().Kind == reflect.Kind.Int {
        return x * 2
    }
    return x
}

func main() {
    println(Twice("ab"), Twice(21), Twice(true))
}
