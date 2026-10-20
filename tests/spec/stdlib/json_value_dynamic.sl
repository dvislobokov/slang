// test: json.ParseValue gives a dynamic json.Value navigable by key and index
// status: pending
namespace spec.stdlib.json_value_dynamic

import std.io
import std.encoding

func Run() void! {
    var v = json.ParseValue("{\"name\": \"x\", \"nums\": [1, 2.5, 3], \"ok\": true, \"none\": null}")?
    println(v.Get("name").String()?, v.Get("nums").Len(), v.Get("nums").Index(1).Float()?)
    println(v.Get("ok").Bool()?, v.Get("none").IsNull(), v.Has("missing"))
    try json.ParseValue("{bad") catch err {
        println("invalid json rejected")
    }
}

func main() {
    try Run() catch err {
        println("error:", err)
    }
}
