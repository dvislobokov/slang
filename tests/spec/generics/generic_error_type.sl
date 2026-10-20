// test: a generic struct implementing error, returned from a fallible generic func
// status: pending
namespace spec.generics.generic_error_type

import std.io
import std

struct NotFound<K> {
    Key K
    func Message() string {
        return fmt.Format("key {0} not found", this.Key)
    }
}

func Lookup<K: Ordered>(m map[K]string, k K) string! {
    if len(m[k]) == 0 {
        return NotFound<K>{Key: k}
    }
    return m[k]
}

func main() {
    var byId = map[int]string{1: "one"}
    var byName = map[string]string{"x": "ex"}
    println(try Lookup(byId, 1) else "?")
    try Lookup(byId, 7) catch err {
        println(err)
    }
    try Lookup(byName, "q") catch err {
        var nf NotFound<string>
        if errors.As(err, &nf) {
            println("missing", nf.Key)
        }
    }
}
