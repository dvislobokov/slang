// test: a try expression inside a block try handles its own error; a block try inside a catch falls back
// status: passing
namespace spec.errors2.block_try_nested_in_try_expr

import std.io

func Get(k string) int! {
    if k == "" {
        return error("empty key")
    }
    if k == "bad" {
        return error("bad key")
    }
    return len(k)
}

func Load(k string) int! {
    return Get(k)?
}

func main() {
    try {
        var a = try Get("") else 100
        var b = Get("abc")?
        println("inner sum", a + b)
        var c = Get("bad")?
        println("not reached", c)
    } catch err {
        println("block caught:", err)
    }
    var v = try Load("bad") catch err {
        println("expr caught:", err)
        try {
            var w = Get("fallback")?
            println("fallback", w)
        } catch err2 {
            println("fallback failed:", err2)
        }
        return
    }
    println(v)
}
