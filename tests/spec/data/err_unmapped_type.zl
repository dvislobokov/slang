// error: field Doc.Owner of type Person cannot be mapped to a column
// status: passing
namespace spec.data.err_unmapped_type

import std
import std.io

struct Person {
    Name string
}

struct Doc {
    Id    int
    Owner Person
}

struct Docs {
    data.Context
    Docs data.Set<Doc>
}

func main() {
    var ctx = data.OpenDB<Docs>(data.Provider.Sqlite, database.sqlite.Open(":memory:"))
    println(ctx.Docs.TableName())
}
