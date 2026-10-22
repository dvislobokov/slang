// test: 100000 appends with += and with strings.Builder give strings of the right length and content
// status: passing
namespace spec.strings2.long_concat

import std
import std.io

func main() {
    var s = ""
    for _ in 0..100000 {
        s += "x"
    }
    println(len(s), s[99999], s[..3])
    var b = strings.Builder{}
    for i in 0..50000 {
        if i % 2 == 0 {
            b.Write("ab")
        } else {
            b.Write("я")
        }
    }
    var t = b.String()
    println(b.Len(), len(t), len(strings.Runes(t)), strings.Count(t, "я"))
}
