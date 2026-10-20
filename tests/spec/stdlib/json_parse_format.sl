// test: json.Parse<T> and json.Format via compile-time reflection, honouring @json
// status: passing
namespace spec.stdlib.json_parse_format

import std.io
import std.encoding

struct User {
    @json(name: "id") Id int
    Name string
    @json(omitEmpty: true) Email string
    Tags []string
}

func Run() void! {
    var u = json.Parse<User>("{\"id\": 7, \"Name\": \"ann\", \"Tags\": [\"a\", \"b\"]}")?
    println(u.Id, u.Name, "[" + u.Email + "]", u.Tags)
    u.Name = "bob"
    println(json.Format(u))
}

func main() {
    try Run() catch err {
        println("error:", err)
    }
}
