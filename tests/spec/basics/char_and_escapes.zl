// test: string and char escapes including \u{...}
// status: passing
namespace spec.basics.char_and_escapes

import std.io

func main() {
    println("tab:[\t] quote:[\"] backslash:[\\]")
    println("\u{41}\u{42}\u{1F600} \u{263A}")
    var c = '\u{263A}'
    var q = '\''
    println(c, q, 'я', '\n' == '\u{a}')
    println(len("\u{1F600}"), len("я"), len("\0"))
    println("line1\nline2")
    println(int('A'), string('z') + "!")
}
