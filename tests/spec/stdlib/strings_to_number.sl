// test: ToInt and ToFloat accept valid input and fail on empty, spaces, garbage and overflow
// status: passing
namespace spec.stdlib.strings_to_number

import std.io
import std

func Int(s string) string {
    var n = try strings.ToInt(s) else -1
    return fmt.Format("{0}", n)
}

func main() {
    println(Int("42"), Int("-7"), Int("0"), Int("007"))
    println(Int(""), Int(" 1"), Int("1x"), Int("9223372036854775808"), Int("-"))
    println(Int("9223372036854775807"), Int("-9223372036854775808"))
    println(try strings.ToFloat("1.5") else -1, try strings.ToFloat("1e3") else -1, try strings.ToFloat("-0.25") else -1)
    println(try strings.ToFloat("abc") else -1, try strings.ToFloat("") else -1)
}
