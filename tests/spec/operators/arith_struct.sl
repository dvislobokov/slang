// test: + - * / % and unary - on a struct call Add/Sub/Mul/Div/Rem/Neg directly; compound assignment uses the binary operator
// status: passing
namespace spec.operators.arith_struct

import std.io

struct V {
    X int
    Y int

    func Add(o V) V { return V{X: this.X + o.X, Y: this.Y + o.Y} }
    func Sub(o V) V { return V{X: this.X - o.X, Y: this.Y - o.Y} }
    func Mul(o V) V { return V{X: this.X * o.X, Y: this.Y * o.Y} }
    func Div(o V) V { return V{X: this.X / o.X, Y: this.Y / o.Y} }
    func Rem(o V) V { return V{X: this.X % o.X, Y: this.Y % o.Y} }
    func Neg() V { return V{X: -this.X, Y: -this.Y} }
}

func show(v V) {
    println(v.X, v.Y)
}

func main() {
    var a = V{X: 7, Y: 9}
    var b = V{X: 2, Y: 4}
    show(a + b)
    show(a - b)
    show(a * b)
    show(a / b)
    show(a % b)
    show(-a)
    // precedence and associativity are those of the builtin operators
    show(a + b * b - -b)
    show((a + b) * b)
    var c = a
    c += b
    c -= V{X: 1, Y: 1}
    c *= b
    show(c)
    // the operands are evaluated left to right, each once
    var vs = [a, b]
    show(vs[0] + vs[1] + vs[0])
}
