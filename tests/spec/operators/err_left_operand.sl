// error: mismatched types int and V for operator *
// status: passing
namespace spec.operators.err_left_operand

struct V {
    X int
    func Mul(k int) V { return V{X: this.X * k} }
}

func main() {
    var v = V{X: 2}
    var k = 3
    _ = v * k
    // the left operand selects the method: int has no Mul taking a V
    _ = k * v
}
