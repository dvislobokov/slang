// test: switch over the caught error by type with default
// status: passing
namespace spec.errors.catch_typed_switch

import std.io

struct NotFound {
    Key string
    func Message() string { return "not found: " + this.Key }
}

struct Denied {
    func Message() string { return "denied" }
}

func Get(k string) string! {
    switch k {
        case "secret":
            return Denied{}
        case "x":
            return error("plain")
        case "a":
            return "A"
    }
    return NotFound{Key: k}
}

func main() {
    for k in ["a", "zz", "secret", "x"] {
        var v = try Get(k) catch err {
            switch err {
                case NotFound nf:
                    println("missing key", nf.Key)
                case Denied:
                    println("no access")
                default:
                    println("other:", err.Message())
            }
            continue
        }
        println("value", v)
    }
}
