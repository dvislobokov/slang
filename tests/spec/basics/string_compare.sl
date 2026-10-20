// test: string comparison is byte-wise lexicographic
// status: passing
namespace spec.basics.string_compare

import std.io

func main() {
    println("abc" < "abd", "Z" < "a", "" < "a", "ab" < "abc")
    println("b" > "abc", "abc" == "abc", "abc" != "abC", "a" <= "a", "b" >= "c")
    var x = "hé"
    var y = "he"
    println(x > y, x == "h" + "é")
}
