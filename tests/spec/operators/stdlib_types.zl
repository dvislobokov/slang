// test: the stdlib types use operators: big.Int/Rat/Decimal arithmetic and comparisons, time.Duration and time.Time arithmetic, complex.Complex, collections.BitSet indexing
// status: passing
// run-flags: --leak-check
namespace spec.operators.stdlib_types

import std
import std.collections
import std.io
import std.math

func main() {
    var f = big.NewInt(1)
    for i in 1..26 {
        f *= big.NewInt(i)
    }
    println(
        f.String(),
        f > big.NewInt(0),
        (f / big.NewInt(1000000)).String(),
        (f % big.NewInt(7)).String(),
        (-f).Sign(),
    )
    var r = big.NewRat(1, 3) + big.NewRat(1, 6)
    println(r.String(), r == big.NewRat(1, 2), r * big.NewRat(2, 1) == big.NewRat(1, 1))
    var price = big.MustParseDecimal("19.99")
    var total = price * big.NewDecimal(3, 0) - big.MustParseDecimal("0.97")
    println(
        total.String(),
        total >= big.MustParseDecimal("59"),
        (try total / big.NewDecimal(4, 0) else big.Decimal{}).String(),
    )
    var d = time.Hours(1) + time.Minutes(30)
    println(d.String(), (d * 2).String(), (d / 3).String(), (-d).String(), d > time.Minutes(89))
    var t0 = time.Unix(1700000000, 0).UTC()
    var t1 = t0 + time.Hours(25)
    println((t1 - t0).String(), t1 > t0, t0 == time.Unix(1700000000, 0))
    var z = complex.New(1, 2) * complex.New(3, 4)
    println(z.String(), (z - complex.New(-5, 10)) == complex.New(0, 0))
    var b = BitSet{}
    b[3] = true
    b[70] = true
    b[3] = false
    b[5] = true
    println(b[3], b[5], b[70], b[1000], b.Items())
}
