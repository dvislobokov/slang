// test: an operator method returning T! makes the operator expression fallible: handled with ?, try ... else and try ... catch
// status: passing
namespace spec.operators.fallible_method

import std.io

struct Frac {
    N int
    D int
    func Div(o Frac) Frac! {
        if o.N == 0 {
            return error("division by zero")
        }
        return Frac{N: this.N * o.D, D: this.D * o.N}
    }
}

func half(f Frac) Frac! {
    return (f / Frac{N: 2, D: 1})?
}

func main() {
    var a = Frac{N: 3, D: 4}
    var h = try half(a) else Frac{}
    println(h.N, h.D)
    var z = try a / Frac{N: 0, D: 1} else Frac{N: -1, D: -1}
    println(z.N, z.D)
    var q = try a / Frac{N: 0, D: 5} catch err {
        println("caught:", err)
        return
    }
    println(q.N)
}
