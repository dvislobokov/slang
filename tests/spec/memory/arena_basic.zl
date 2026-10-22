// test: arena.New() allocates objects that are all released at once by Free
// status: passing
namespace spec.memory.arena_basic

import std
import std.io

struct Point {
    X int
    Y int
}

func main() {
    var a = arena.New()
    var total = 0
    for i in 0..100 {
        var p = a.New<Point>()
        p.X = i
        p.Y = 1
        total += p.X + p.Y
    }
    println("total", total)
    a.Free()
    println("arena freed")
}
