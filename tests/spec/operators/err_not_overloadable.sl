// error: only + - * / % @ == != < <= > >= and indexing can be overloaded
// status: passing
namespace spec.operators.err_not_overloadable

struct Flags {
    Bits int
    func And(o Flags) Flags { return Flags{Bits: this.Bits & o.Bits} }
}

func main() {
    var a = Flags{Bits: 3}
    _ = a & a
}
