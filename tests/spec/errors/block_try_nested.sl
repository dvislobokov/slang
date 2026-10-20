// test: nested block try: ? goes to the nearest catch; the outer block continues
// status: passing
namespace spec.errors.block_try_nested

import std.io

func Fail(msg string) void! {
    return error(msg)
}

func Ok() void! {
}

func main() {
    try {
        Ok()?
        try {
            Fail("inner")?
            println("unreachable 1")
        } catch err {
            println("inner catch:", err)
        }
        println("outer continues")
        Fail("outer")?
        println("unreachable 2")
    } catch err {
        println("outer catch:", err)
    }
    println("done")
}
