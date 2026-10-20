// test: Fields splits on runs of whitespace; SplitN limits the number of parts
// status: passing
namespace spec.stdlib.strings_fields_splitn

import std
import std.io

func main() {
    var f = strings.Fields("  a b\t\tc\n d  ")
    println(len(f), f, len(strings.Fields("   ")))
    println(
        strings.SplitN("a,b,c,d", ",", 2),
        strings.SplitN("a,b", ",", 5),
        len(strings.SplitN("a,b", ",", 0)),
    )
    println(strings.SplitN("a,b,c", ",", -1))
}
