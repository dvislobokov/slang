// test: variants carry fields; data and plain variants mix; switch destructures them
// status: passing
namespace spec.enums.data_variants

import std.io

enum Expr {
    Num(v int)
    Add(a int, b int)
    Neg(v int)
    Zero
}

func Eval(e Expr) int {
    switch e {
        case Expr.Num(v):
            return v
        case Expr.Add(a, b):
            return a + b
        case Expr.Neg(v):
            return -v
        case Expr.Zero:
            return 0
    }
    return -999
}

func main() {
    var list = [Expr.Num(4), Expr.Add(2, 3), Expr.Neg(7), Expr.Zero]
    for e in list {
        println(Eval(e))
    }
}
