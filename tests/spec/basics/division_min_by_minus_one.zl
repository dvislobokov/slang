// test: MinInt / -1 wraps to MinInt and MinInt % -1 is 0 (Go semantics, no trap)
// status: passing
namespace spec.basics.division_min_by_minus_one

import std.io

func main() {
    var m int = -9223372036854775807 - 1
    var d = -1
    println(m / d, m % d)
    var s int8 = -128
    var t int8 = -1
    println(s / t, s % t)
}
