// test: compile-time reflect.Fields<T>() in a generic encoder honours @json name and skip
// status: pending
namespace spec.reflection.compile_time_fields_json

import std.io
import std

struct User {
    @json(name: "id") Id int
    Email string
    @json(skip: true) cache string
    Admin bool
}

func Scalar(x any) string {
    switch x {
        case string s:
            return "\"" + s + "\""
        case int n:
            return fmt.Format("{0}", n)
        case bool b:
            return fmt.Format("{0}", b)
    }
    return "null"
}

func ToJson<T>(value T) string {
    var b = strings.Builder{}
    b.Write("{")
    var first = true
    for f in reflect.Fields<T>() {
        var tag = f.Attribute<json>()
        if tag != nil && tag.skip {
            continue
        }
        var name = f.Name
        if tag != nil && tag.name != "" {
            name = tag.name
        }
        if !first {
            b.Write(",")
        }
        first = false
        b.Write(fmt.Format("\"{0}\":", name))
        b.Write(Scalar(f.Get(value)))
    }
    b.Write("}")
    return b.String()
}

func main() {
    var u = User{Id: 7, Email: "a@b.c", cache: "secret", Admin: true}
    println(ToJson(u))
}
