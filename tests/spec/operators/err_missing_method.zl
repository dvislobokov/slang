// error: operator / is not defined on Money: Money has no method Div
// status: passing
namespace spec.operators.err_missing_method

struct Money {
    Cents int
    func Add(o Money) Money { return Money{Cents: this.Cents + o.Cents} }
}

func main() {
    var m = Money{Cents: 5}
    var n = m + m
    _ = n / m
}
