// error: cannot use string as int in argument 1 of V.Mul (operator *)
// status: passing
namespace spec.operators.err_param_type

struct V {
    X int
    func Mul(k int) V { return V{X: this.X * k} }
}

func main() {
    var v = V{X: 2}
    _ = v * "3"
}
