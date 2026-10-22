// test: switch on char values (lists and ranges) and on bool
// status: passing
namespace spec.switch.on_chars_and_bools

import std.io

func Class(c char) string {
    switch c {
        case 'a', 'e', 'i', 'o', 'u':
            return "vowel"
        case '0'..':':
            return "digit"
        case ' ':
            return "space"
        default:
            return "other"
    }
}

func main() {
    var out = ""
    for ch in "hi 42" {
        out += Class(ch) + ","
    }
    println(out)
    var b = len(out) > 10
    switch b {
        case true:
            println("long")
        case false:
            println("short")
    }
}
