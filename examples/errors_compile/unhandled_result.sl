// error: result of type int! must be handled
namespace examples.bad

import std.io

func Parse(s string) int! {
    if s == "" {
        return error("empty")
    }
    return 1
}

func main() {
    var n = Parse("1")
    println(n)
}
