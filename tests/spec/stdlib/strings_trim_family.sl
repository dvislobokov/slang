// test: Trim (whitespace), TrimPrefix/TrimSuffix (one occurrence), TrimLeft/TrimRight (cutset)
// status: passing
namespace spec.stdlib.strings_trim_family

import std.io
import std

func main() {
    println("[" + strings.Trim("  \t hi there \n ") + "]", "[" + strings.Trim("   ") + "]")
    println(strings.TrimPrefix("prefix-body", "prefix-"), strings.TrimPrefix("body", "x"), strings.TrimPrefix("aaab", "a"))
    println(strings.TrimSuffix("file.tar.gz", ".gz"), strings.TrimSuffix("file", ".gz"))
    println(strings.TrimLeft("xxyhixy", "xy"), strings.TrimRight("xxyhixy", "xy"), "[" + strings.TrimLeft("aaa", "a") + "]")
}
