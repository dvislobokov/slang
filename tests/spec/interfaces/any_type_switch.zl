// test: any holds values of every type; a type switch binds the concrete value
// status: passing
namespace spec.interfaces.any_type_switch

import std.io

struct User {
    Name string
}

func Describe(x any) string {
    switch x {
        case int n:
            return "int " + string(char(48 + n))
        case string s:
            return "string " + s
        case bool b:
            if b {
                return "bool true"
            }
            return "bool false"
        case User u:
            return "user " + u.Name
        case *User p:
            return "user ptr " + p.Name
        default:
            return "unknown"
    }
}

func main() {
    var items []any = [7, "hi", true, User{Name: "ann"}, new User{Name: "bob"}, 2.5]
    for it in items {
        println(Describe(it))
    }
}
