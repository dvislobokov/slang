// test: s[a..b] is a byte-range substring
// status: passing
namespace spec.basics.string_slice

import std.io

func main() {
    var s = "hello, world"
    println(s[0..5], "|" + s[5..7] + "|", s[7..len(s)])
    println(s[..5], s[7..])
}
