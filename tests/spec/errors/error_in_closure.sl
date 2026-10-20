// test: closures may be fallible and use ? inside
// status: passing
namespace spec.errors.error_in_closure

import std
import std.io

func main() {
    var parse = func(s string) int! {
        var n = strings.ToInt(s)?("parse " + s)
        return n * 2
    }
    println(try parse("21") else 0)
    try parse("q") catch err {
        println(strings.HasPrefix(err.Message(), "parse q: "))
    }
}
