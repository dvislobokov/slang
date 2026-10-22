// error: catch block must return when the value of try is used
namespace examples.bad

import std.io

func Parse(s string) int! {
    if s == "" {
        return error("empty")
    }
    return 1
}

func main() {
    var n = try Parse("") catch err {
        println("failed:", err)
    }
    println(n)
}
