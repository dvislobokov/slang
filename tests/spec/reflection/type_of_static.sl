// test: reflect.TypeOf<T>() gives Name, FullName and Kind without a value
// status: pending
namespace spec.reflection.type_of_static

import std.io
import std

struct User {
    Name string
}

enum Color { Red, Green }

func main() {
    var t = reflect.TypeOf<User>()
    println(t.Name, t.FullName, t.Kind)
    println(t.Kind == reflect.Kind.Struct)
    var c = reflect.TypeOf<Color>()
    println(c.Name, c.FullName, c.Kind)
    println(reflect.TypeOf<int>().Name, reflect.TypeOf<int>().Kind)
}
