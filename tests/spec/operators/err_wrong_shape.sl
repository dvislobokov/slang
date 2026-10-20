// error: operator + on P needs method Add of the form `func Add(other P) P`, found `func Add(P)`
// status: passing
namespace spec.operators.err_wrong_shape

struct P {
    X int
    // a mutating Add (like List.Add) is not an operator method
    func Add(o P) {
        this.X += o.X
    }
}

func main() {
    var p = P{X: 1}
    _ = p + p
}
