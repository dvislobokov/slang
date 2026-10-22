// test: escapes in literals produce the expected bytes, and %q quotes them back
// status: passing
namespace spec.strings2.escapes_roundtrip

import std
import std.io

func main() {
    var s = "a\tb\n\"c\"\\"
    println(len(s))
    println(fmt.Sprintf("%q", s))
    println(fmt.Sprintf("%q", "é\u{1F600}"))
    var bytes = strings.Bytes("\u{e9}\0")
    println(bytes)
}
