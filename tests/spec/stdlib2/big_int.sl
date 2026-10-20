// test: std.math.big.Int arithmetic, division modes, Exp/ModInverse/GCD/Sqrt, parsing, bytes and ProbablyPrime
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.big_int

import std
import std.io
import std.math

func main() {
    var a = big.MustParseInt("123456789012345678901234567890")
    var b = big.NewInt(-987654321)
    println(a.String(), b.String(), a.Add(b).String(), a.Sub(b).String(), a.Mul(b).String())
    println(a.Quo(b).String(), a.Rem(b).String(), a.Div(b).String(), a.Mod(b).String())
    var m7 = big.NewInt(-7)
    var d2 = big.NewInt(2)
    println(m7.Quo(d2).String(), m7.Rem(d2).String(), m7.Div(d2).String(), m7.Mod(d2).String(), m7.Rsh(1).String(), big.NewInt(7).Rsh(1).String())
    var f = big.NewInt(1)
    for i in 1..31 {
        f = f.Mul(big.NewInt(i))
    }
    println("30! =", f.String(), f.BitLen(), f.TrailingZeroBits())
    var two = big.NewInt(2)
    var p = two.Pow(521).Sub(big.NewInt(1))
    println(p.Text(16) == "1" + strings.Repeat("ff", 65), p.ProbablyPrime(10), p.Sub(two).ProbablyPrime(10))
    println(big.NewInt(97).ProbablyPrime(5), big.NewInt(91).ProbablyPrime(5), big.NewInt(2).ProbablyPrime(0), big.NewInt(1).ProbablyPrime(0))
    var c = big.MustParseInt("561")
    println("carmichael 561 prime:", c.ProbablyPrime(1))
    var x = try big.NewInt(4).Exp(big.NewInt(13), big.NewInt(497)) else big.Int{}
    println(x.String())
    var inv = try big.NewInt(3).ModInverse(big.NewInt(11)) else big.Int{}
    println(inv.String())
    try {
        _ = big.NewInt(2).ModInverse(big.NewInt(4))?
    } catch err {
        println(err)
    }
    init2()
}

func digest(x big.Int) string {
    var s = x.String()
    return fmt.Format("{0}:{1}..{2}", len(s), s[..12], s[len(s) - 12..])
}

func init2() {
    var x = big.NewInt(3).Pow(4000)
    var y = big.NewInt(7).Pow(3000)
    var p = x.Mul(y)
    println(digest(p))
    var (q, r) = p.Sub(big.NewInt(12345)).QuoRem(big.NewInt(5).Pow(1800).Add(big.NewInt(1)))
    println(digest(q), digest(r))
    println(q.Mul(big.NewInt(5).Pow(1800).Add(big.NewInt(1))).Add(r).Equals(p.Sub(big.NewInt(12345))))
    println(digest(p.Sqrt()), p.Sqrt().Mul(p.Sqrt()).Cmp(p) <= 0)
    println(big.NewInt(12).GCD(big.NewInt(-18)).String(), big.NewInt(-5).Abs().String(), big.NewInt(0).Sign(), big.NewInt(-3).Sign())
    var h = try big.ParseInt("-0xDEAD_beef", 0) else big.Int{}
    println(h.String(), h.Text(16), h.Text(2), h.Int64(), h.IsInt64(), big.FromUint64(18446744073709551615).String(), big.FromUint64(18446744073709551615).IsInt64())
    var bad = try big.ParseInt("12x", 10) catch err {
        println(err)
        return
    }
    println(bad.String())
}
