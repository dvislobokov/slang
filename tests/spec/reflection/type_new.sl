// test: t.New() returns a fresh zero value on the heap as any holding *T
// status: passing
namespace spec.reflection.type_new

import std.io
import std

struct User {
    Name string
    Age int
}

func main() {
    var fresh = reflect.TypeOf<User>().New()
    switch fresh {
        case *User p:
            p.Name = "new"
            println("got *User", p.Name, p.Age)
        default:
            println("wrong type")
    }
}
