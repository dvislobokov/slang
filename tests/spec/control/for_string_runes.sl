// test: for ch in s iterates code points; for i, ch in s gives the byte offset
// status: passing
namespace spec.control.for_string_runes

import std.io
import std

func main() {
    var s = "aé😀b"
    for ch in s {
        print(string(ch) + "|")
    }
    println()
    for i, ch in s {
        print(fmt.Format("{0}:{1} ", i, ch))
    }
    println()
}
