// test: s[i] yields the byte at index i (type byte, prints as a number)
// status: passing
namespace spec.basics.string_index_byte

import std.io

func main() {
    var s = "héllo"
    println(s[0], s[1], s[2], s[len(s) - 1])
    var b byte = s[0]
    println(b == 104)
    var sum = 0
    for i in 0..len(s) {
        sum += int(s[i])
    }
    println(sum)
}
