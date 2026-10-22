// error: slicing cannot be overloaded
// status: passing
namespace spec.operators.err_slice_overloaded

struct Table {
    vals []int
    func Index(i int) int { return this.vals[i] }
}

func main() {
    var t = Table{vals: [1, 2]}
    _ = t[0..1]
}
