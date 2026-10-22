// test: attribute parameters may be enums and slices of constants
// status: passing
namespace spec.attributes2.enum_and_slice_params

import std
import std.io

enum Level {
    Debug
    Info
    Warn
}

@attribute
struct logged {
    level Level = Level.Info
    tags  []string
}

@logged(level: Level.Warn, tags: ["db", "slow"])
struct Query {
    Sql string
}

@logged(tags: [])
struct Ping {
    N int
}

func main() {
    var q = reflect.TypeOf<Query>().Attribute<logged>()
    println(q.level, q.tags, len(q.tags))
    var p = reflect.TypeOf<Ping>().Attribute<logged>()
    println(p.level, len(p.tags))
}
