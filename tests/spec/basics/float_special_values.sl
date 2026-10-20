// test: NaN, +Inf, -Inf and negative zero print like Go
// status: passing
namespace spec.basics.float_special_values

import std.io
import std

func main() {
    var zero = 0.0
    println(math.NaN(), math.Inf(1), math.Inf(-1))
    println(-zero, zero, 1.0 / zero, -1.0 / zero)
    var n = math.NaN()
    println(n == n, n != n, math.IsNaN(zero / zero))
}
