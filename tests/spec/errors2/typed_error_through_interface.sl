// test: a typed error returned through an interface method is matched with a type switch and errors.As after wrapping
// status: passing
namespace spec.errors2.typed_error_through_interface

import std
import std.io

struct NotFound {
    Key string
    func Message() string { return "not found: " + this.Key }
}

interface Store {
    func Get(key string) string!
}

struct MemStore {
    data map[string]string
    func Get(key string) string! {
        if this.data[key] == "" {
            return NotFound{Key: key}
        }
        return this.data[key]
    }
}

func Lookup(s Store, key string) string! {
    return s.Get(key)?("lookup")
}

func main() {
    var s Store = MemStore{data: map[string]string{"a": "1"}}
    println(try Lookup(s, "a") else "?")
    try s.Get("b") catch err {
        switch err {
            case NotFound nf: println("direct", nf.Key)
            default:
                println("other")
        }
    }
    try Lookup(s, "c") catch err {
        println(err)
        var nf NotFound
        if errors.As(err, &nf) {
            println("as", nf.Key)
        }
    }
}
