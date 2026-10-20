// error: type Meters does not satisfy constraint Mul: method Mul is `func Mul(int) Meters`, expected `func Mul(other Meters) Meters`
// status: passing
namespace spec.operators.err_constraint_shape

struct Meters {
    N int
    func Mul(k int) Meters { return Meters{N: this.N * k} }
}

func Square<T: Mul>(x T) T {
    return x * x
}

func main() {
    _ = Square(Meters{N: 2})
}
