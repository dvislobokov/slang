// test: operator methods on a generic struct Vec2<T: Add>: the instance's Add uses T's +, so Vec2<int>, Vec2<float> and Vec2<Money> all have +
// status: passing
namespace spec.operators.generic_struct

import std.io

struct Money {
    Cents int
    func Add(o Money) Money { return Money{Cents: this.Cents + o.Cents} }
}

struct Vec2<T: Add> {
    X T
    Y T
    func Add(o Vec2<T>) Vec2<T> { return Vec2<T>{X: this.X + o.X, Y: this.Y + o.Y} }
}

func Total<T: Add>(a T, b T, c T) T {
    return a + b + c
}

func main() {
    var a = Vec2<int>{X: 1, Y: 2} + Vec2<int>{X: 10, Y: 20}
    println(a.X, a.Y)
    var f = Vec2<float>{X: 0.5, Y: 1} + Vec2<float>{X: 0.25, Y: 1}
    println(f.X, f.Y)
    var m = Vec2<Money>{X: Money{Cents: 1}, Y: Money{Cents: 2}} +
        Vec2<Money>{X: Money{Cents: 3}, Y: Money{Cents: 4}}
    println(m.X.Cents, m.Y.Cents)
    // inference through the operator method: Total<Vec2<int>>
    var t = Total(a, a, Vec2<int>{X: 100, Y: 200})
    println(t.X, t.Y)
}
