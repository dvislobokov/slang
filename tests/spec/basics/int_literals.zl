// test: decimal, hex, binary, octal literals with _ separators
// status: passing
namespace spec.basics.int_literals

import std.io

func main() {
    println(1_000_000, 0xff, 0XFF, 0b1010, 0o17, 0x_ff)
    println(-42, 0, 007)
    println(9223372036854775807)
    var big int64 = -9223372036854775807 - 1
    println(big)
}
