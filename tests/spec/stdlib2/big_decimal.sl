// test: std.math.big.Decimal fixed-point arithmetic (operators; `/` and `%` are fallible) with rounding modes, and Rat
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.big_decimal

import std
import std.io
import std.math

func d(s string) big.Decimal {
    return big.MustParseDecimal(s)
}

func main() {
    var price = d("19.99")
    var qty = big.NewDecimal(3, 0)
    var subtotal = price * qty
    var tax = (subtotal * d("0.0825")).Round(2, big.RoundingMode.HalfUp)
    println(subtotal.String(), tax.String(), (subtotal + tax).String(), subtotal.Scale())
    println((d("0.1") + d("0.2")).String(), d("0.1") + d("0.2") == d("0.3"), d("1.50").Cmp(d("1.5")), d("-2").Cmp(d("1")))
    println((d("1") - d("0.001")).String(), d("-12.340").String(), d("12.340").Normalize().String(), d(".5").String(), d("1e-3").String(), d("2.5E2").String(), d("-0.0").String())
    var modes = [big.RoundingMode.HalfUp, big.RoundingMode.HalfEven, big.RoundingMode.HalfDown, big.RoundingMode.Down, big.RoundingMode.Up, big.RoundingMode.Ceiling, big.RoundingMode.Floor]
    for v in ["2.5", "3.5", "-2.5", "2.4", "-2.6", "2.51"] {
        print(v + ":")
        for m in modes {
            print(" " + d(v).Round(0, m).String())
        }
        println()
    }
    var third = try d("1").DivRound(d("3"), 10, big.RoundingMode.HalfEven) else big.Decimal{}
    var twoThirds = try d("2").DivRound(d("3"), 4, big.RoundingMode.HalfUp) else big.Decimal{}
    var q = try d("10") / d("4") else big.Decimal{}
    println(third.String(), twoThirds.String(), q.String(), q.Normalize().String())
    try {
        _ = (d("1") / d("0"))?
    } catch err {
        println(err)
    }
    try {
        _ = big.ParseDecimal("1.2.3")?
    } catch err {
        println(err)
    }
    println(d("1234.5678").Round(-2, big.RoundingMode.HalfUp).String(), d("1234.5678").Truncate(1).String(), d("-7.5").IntPart().String(), (try d("7.25") % d("-2") else d("0")).String())
    println(d("3.14159").Float64(), d("100").Rescale(2).String(), d("-1.5").Abs().String(), (-d("1.5")).String(), d("0").Sign(), d("-0.1").Sign())

    var r = big.NewRat(6, -8)
    println(r.String(), r.RatString(), r.Num().String(), r.Denom().String(), r.Sign())
    var s = r + big.NewRat(1, 4)
    println(s.String(), s.RatString(), s.IsInt(), (r * big.NewRat(4, 3)).RatString(), (r / big.NewRat(3, 2)).String(), r.Inv().String(), (r - r).RatString())
    println(big.NewRat(1, 3).FloatString(5), big.NewRat(2, 3).FloatString(0), big.NewRat(-1, 8).FloatString(2), big.NewRat(1, 3).Cmp(big.NewRat(1, 2)), big.NewRat(1, 3) < big.NewRat(1, 2), -big.NewRat(1, 2) == big.NewRat(-2, 4), big.NewRat(1, 4).Float64())
    var pr = try big.ParseRat("10/4") else big.Rat{}
    var pd = try big.ParseRat("-1.25") else big.Rat{}
    println(pr.String(), pd.String(), big.Rat{}.String())
    try {
        _ = big.ParseRat("1/0")?
    } catch err {
        println(err)
    }
}
