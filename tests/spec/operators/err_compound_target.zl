// error: compound assignment with an overloaded operator += needs a variable or field on the left
// status: passing
namespace spec.operators.err_compound_target

struct C {
    N int
    func Add(o C) C { return C{N: this.N + o.N} }
}

func pick(cs []C, i int) []C {
    return cs
}

func main() {
    var cs = [C{N: 1}]
    var i = 0
    pick(cs, i)[i + 0] += C{N: 2}
}
