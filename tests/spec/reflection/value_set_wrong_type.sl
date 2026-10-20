// test: Value.Set with a value of the wrong type panics at run time (exit code 2)
// status: pending
// exit: 2
// stderr: Email
namespace spec.reflection.value_set_wrong_type

import std.io
import std

struct User {
    Email string
}

func main() {
    var u = User{}
    println("setting")
    reflect.ValueOf(&u).Field("Email").Set(42)
    println("unreachable")
}
