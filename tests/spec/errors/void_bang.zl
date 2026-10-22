// test: void! functions may fall off the end (success) or return an error; ? works as a statement
// status: passing
namespace spec.errors.void_bang

import std.io

func Check(n int) void! {
    if n < 0 {
        return error("negative")
    }
    println("ok", n)
}

func CheckAll(ns []int) void! {
    for n in ns {
        Check(n)?
    }
    println("all ok")
}

func main() {
    try CheckAll([1, 2]) catch err {
        println("unexpected", err)
    }
    try CheckAll([3, -1, 4]) catch err {
        println("failed:", err)
    }
}
