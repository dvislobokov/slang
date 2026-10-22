// test: the right operand takes the type of the method's parameter (Mul(k int) scales); the result type is the method's (Sub of two Points is a Delta)
// status: passing
namespace spec.operators.mixed_operand

import std.io

struct Delta {
    DX int
    DY int
    func Mul(k int) Delta { return Delta{DX: this.DX * k, DY: this.DY * k} }
}

struct Point {
    X int
    Y int
    func Add(d Delta) Point { return Point{X: this.X + d.DX, Y: this.Y + d.DY} }
    func Sub(o Point) Delta { return Delta{DX: this.X - o.X, DY: this.Y - o.Y} }
}

func main() {
    var p = Point{X: 1, Y: 2}
    var q = Point{X: 4, Y: 8}
    var d = q - p
    println(d.DX, d.DY)
    var e = d * 3
    println(e.DX, e.DY)
    var r = p + d * 2
    println(r.X, r.Y)
    p += Delta{DX: 10, DY: 10}
    println(p.X, p.Y)
    var k int8 = 2
    var f = d * int(k)
    println(f.DX, f.DY)
}
