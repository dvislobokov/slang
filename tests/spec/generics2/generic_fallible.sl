// test: generics combine with T!: a generic function returning T! and taking func(string) T!
// status: passing
namespace spec.generics2.generic_fallible

import std.io
import std

func ParseAll<T>(xs []string, parse func(string) T!) []T! {
    var out []T = []
    for x in xs {
        out = append(out, parse(x)?("item " + x))
    }
    return out
}

func First<T>(xs []T) T! {
    if len(xs) == 0 {
        return error("empty")
    }
    return xs[0]
}

func ParseBool(s string) bool! {
    if s == "yes" { return true }
    if s == "no" { return false }
    return error("not a bool: " + s)
}

func main() {
    var ns = try ParseAll(["1", "22", "333"], func(s string) int! { return strings.ToInt(s)? }) catch err {
        println("unexpected", err)
        return
    }
    println(ns)
    var bs = try ParseAll(["yes", "maybe"], ParseBool) catch err {
        println("error:", err)
        var e []int = []
        println(try First(e) else -1, try First(["z"]) else "none")
        return
    }
    println(bs)
}
