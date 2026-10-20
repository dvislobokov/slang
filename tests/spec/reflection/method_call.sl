// test: t.Method(name) finds a method; Call invokes it with a receiver; unknown names give nil
// status: pending
namespace spec.reflection.method_call

import std.io
import std

struct User {
    Name string

    func Greet() string {
        return "hi " + this.Name
    }

    func Store() void! {
        if this.Name == "" {
            return error("empty name")
        }
    }
}

func Run() void! {
    var u = User{Name: "ann"}
    var t = reflect.TypeOf<User>()
    var res = t.Method("Greet").Call(&u)?
    switch res {
        case string s:
            println(s)
    }
    println(t.Method("Missing") == nil)
    var empty = User{}
    try t.Method("Store").Call(&empty) catch err {
        println("Store failed:", err)
    }
}

func main() {
    try Run() catch err {
        println("unexpected", err)
    }
}
