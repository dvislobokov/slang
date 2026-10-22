// test: Runes/FromRunes and Bytes/FromBytes conversions, Compare ordering
// status: passing
namespace spec.stdlib.strings_runes_bytes

import std
import std.io

func main() {
    var r = strings.Runes("hé!")
    println(len(r), r, strings.FromRunes(r))
    var b = strings.Bytes("hé")
    println(len(b), b, strings.FromBytes(b))
    println(strings.Compare("a", "b"), strings.Compare("b", "a"), strings.Compare("x", "x"))
}
