// error: type Box does not satisfy constraint Add: missing method Add
// status: passing
namespace spec.operators.err_constraint_unsatisfied

struct Box {
    N int
}

func Sum<T: Add>(xs []T) T {
    var acc T
    for x in xs {
        acc = acc + x
    }
    return acc
}

func main() {
    _ = Sum([Box{N: 1}])
}
