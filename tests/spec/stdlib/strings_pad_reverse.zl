// test: PadLeft/PadRight pad to a width in characters; Reverse reverses code points
// status: passing
namespace spec.stdlib.strings_pad_reverse

import std
import std.io

func main() {
    println(
        strings.PadLeft("7", 3, "0"),
        strings.PadRight("ab", 5, ".") + "|",
        strings.PadLeft("long", 2, "x"),
    )
    println(strings.PadLeft("é", 3, "*"), strings.PadRight("", 2, "-"))
    println(strings.Reverse("héllo"), "[" + strings.Reverse("") + "]", strings.Reverse("ab😀"))
}
