// test: a @ b calls MatMul; @ has the precedence of *, @= is its compound form, and @ at the start of a declaration is still an attribute
// status: passing
namespace spec.operators.matmul

import std.io

// 2x2 integer matrix
@deprecated("only for the test")
struct M {
    A int
    B int
    C int
    D int

    func MatMul(o M) M {
        return M{
            A: this.A * o.A + this.B * o.C,
            B: this.A * o.B + this.B * o.D,
            C: this.C * o.A + this.D * o.C,
            D: this.C * o.B + this.D * o.D,
        }
    }
    func Add(o M) M { return M{A: this.A + o.A, B: this.B + o.B, C: this.C + o.C, D: this.D + o.D} }
}

func show(m M) {
    println(m.A, m.B, m.C, m.D)
}

func main() {
    var f = M{A: 1, B: 1, C: 1, D: 0}
    show(f @ f)
    show(f @ f @ f)
    var id = M{A: 1, D: 1}
    // `@` binds like `*`: id + (f @ f)
    show(id + f @ f)
    var p = f
    for _ in 0..8 {
        p @= f
    }
    show(p)
}
