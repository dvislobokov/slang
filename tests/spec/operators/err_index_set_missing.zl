// error: operator []= is not defined on Table: Table has no method IndexSet
// status: passing
namespace spec.operators.err_index_set_missing

struct Table {
    vals []int
    func Index(i int) int { return this.vals[i] }
}

func main() {
    var t = Table{vals: [1, 2]}
    _ = t[0]
    t[1] = 5
}
