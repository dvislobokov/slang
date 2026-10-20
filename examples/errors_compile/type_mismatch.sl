// error: cannot assign string to int
namespace examples.bad

import std.io

func main() {
    var n int = 0
    n = "five"
    println(n)
}
