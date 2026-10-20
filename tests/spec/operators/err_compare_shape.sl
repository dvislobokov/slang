// error: operator < on Age needs method Compare of the form `func Compare(other Age) int`
// status: passing
namespace spec.operators.err_compare_shape

struct Age {
    Years int
    func Compare(o Age) bool { return this.Years < o.Years }
}

func main() {
    var a = Age{Years: 3}
    _ = a < a
}
