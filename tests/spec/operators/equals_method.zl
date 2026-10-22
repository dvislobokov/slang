// test: == and != call Equals when the type has it; without it the builtin rules stay (plain enums by tag, pointers by address)
// status: passing
namespace spec.operators.equals_method

import std.io

struct Money {
    Cents    int
    Currency string
    func Equals(o Money) bool { return this.Cents == o.Cents && this.Currency == o.Currency }
}

enum Color {
    Red
    Green
}

enum Shape {
    Circle(r int)
    Square(side int)

    func Equals(o Shape) bool {
        switch this {
            case Shape.Circle(a):
                switch o {
                    case Shape.Circle(b): return a == b
                    default:
                        return false
                }
            case Shape.Square(a):
                switch o {
                    case Shape.Square(b): return a == b
                    default:
                        return false
                }
        }
    }
}

func main() {
    var a = Money{Cents: 100, Currency: "EUR"}
    var b = Money{Cents: 100, Currency: "EUR"}
    var c = Money{Cents: 100, Currency: "USD"}
    println(a == b, a != b, a == c, a != c)
    // pointers still compare addresses
    var pa = &a
    var pb = &b
    println(pa == pb, pa == pa, *pa == *pb)
    println(Color.Red == Color.Red, Color.Red != Color.Green)
    println(
        Shape.Circle(1) == Shape.Circle(1),
        Shape.Circle(1) == Shape.Square(1),
        Shape.Square(2) != Shape.Square(3),
    )
}
