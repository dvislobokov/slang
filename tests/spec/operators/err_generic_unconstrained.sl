// error: operator * is not defined on type parameter T: its constraint does not allow it (constrain it: T: Mul)
// status: passing
namespace spec.operators.err_generic_unconstrained

func Square<T>(x T) T {
    return x * x
}

func main() {
    _ = Square(2)
}
