// test: Round (half away from zero), Trunc, Mod (sign of dividend), Hypot, Log2/Log10, integer helpers
// status: passing
namespace spec.stdlib.math_round_trunc

import std
import std.io

func main() {
    println(math.Round(2.5), math.Round(-2.5), math.Round(2.4), math.Round(-0.4))
    println(math.Trunc(2.7), math.Trunc(-2.7), math.Mod(7, 3), math.Mod(-7, 3))
    println(math.Hypot(3, 4), math.Log2(8), math.Log10(1000))
    println(math.IMin(3, -4), math.IMax(3, -4), math.IAbs(-9))
    println(math.IsInf(math.Inf(1), 1), math.IsInf(math.Inf(-1), 1), math.IsInf(math.Inf(-1), 0))
}
