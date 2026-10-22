// test: string comparison is bytewise (UTF-8 order equals code point order)
// status: passing
namespace spec.strings2.unicode_compare

import std
import std.io

func main() {
    println("а" < "б", "я" > "а", "z" < "а", "é" > "e", "ё" > "я")
    println(strings.Compare("日", "本"), strings.Compare("abc", "abc"), strings.Compare("Б", "Аб"))
    println("é" == "\u{e9}", "e\u{301}" == "é")
}
