// test: reflect.ValueOf(&u).Field(name) reads and writes a field through the pointer
// status: passing
namespace spec.reflection.value_field_set_get

import std.io
import std

struct User {
    Name string
    Email string
    Age int
}

func main() {
    var u = User{Name: "ann", Age: 30}
    var v = reflect.ValueOf(&u)
    v.Field("Email").Set("a@b.c")
    v.Field("Age").Set(31)
    println(u.Email, u.Age)
    println(v.Field("Name").Get<string>(), v.Field("Age").Get<int>())
}
