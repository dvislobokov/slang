// test: structs are values: assignment, parameters and slice reads copy
// status: passing
namespace spec.structs.value_semantics

import std.io

struct P {
    X int
    Y int
}

func Bump(p P) P {
    p.X += 100
    return p
}

func main() {
    var a = P{X: 1, Y: 2}
    var b = a
    b.X = 9
    var c = Bump(a)
    println(a.X, b.X, c.X)
    var list = [a, b]
    var first = list[0]
    first.Y = 77
    println(list[0].Y, first.Y)
    list[1].Y = 55
    println(list[1].Y, b.Y)
}
