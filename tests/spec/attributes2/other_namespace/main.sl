// test: an attribute struct from a child namespace is used by its path (meta.Table, no import needed inside the parent); it must be exported, with exported fields
// status: passing
namespace spec.attributes2.other_namespace

import std.io
import std

@meta.Table("users")
struct User {
    @meta.Column(Name: "user_id") Id int
    Name string
}

func main() {
    var t = reflect.TypeOf<User>()
    println(t.Attribute<meta.Table>().Name)
    for f in t.Fields {
        var c = f.Attribute<meta.Column>()
        if c != nil {
            println(f.Name, c.Name, c.Nullable)
        }
    }
}
