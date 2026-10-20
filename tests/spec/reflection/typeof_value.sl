// test: builtin typeof(x) returns the static type of a value, or the dynamic type of an interface value
// status: passing
namespace spec.reflection.typeof_value

import std.io
import std

struct User {
    Name string
}

func Dynamic(x any) string {
    return typeof(x).Name
}

func main() {
    var u = User{Name: "ann"}
    println(typeof(u).Name, typeof(&u).Name, typeof(&u).Kind)
    println(typeof(5).Name, typeof("s").Name, typeof([1, 2]).Name, typeof([1, 2]).Kind)
    println(Dynamic(u), Dynamic(2.5), Dynamic(map[string]int{}))
    println(typeof(u) == reflect.TypeOf<User>())
}
