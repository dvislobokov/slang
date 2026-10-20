// test: a fallible call in a deferred closure is handled there; defers still run on error returns
// status: passing
namespace spec.errors.error_in_defer

import std.io

func Close(name string) void! {
    if name == "b" {
        return error("close " + name + " failed")
    }
    println("closed", name)
}

func Use(name string) int! {
    defer func() {
        try Close(name) catch err {
            println("in defer:", err)
        }
    }()
    if name == "c" {
        return error("use failed")
    }
    return len(name)
}

func main() {
    for n in ["a", "b", "c"] {
        var r = try Use(n) catch err {
            println("Use:", err)
            continue
        }
        println("result", r)
    }
}
