// error: constrain it: T: Neg
// status: passing
namespace spec.operators.err_neg_unconstrained

func Flip<T: Add>(x T) T {
    return -x
}

func main() {
    _ = Flip(2)
}
