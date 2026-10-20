// test: raw string literals in backticks keep backslashes and newlines as written
// status: passing
namespace spec.basics.raw_string

import std.io

func main() {
    var path = `C:\Users\slang\new`
    println(path, len(path))
    var re = `\d+\.\d+`
    println(re)
    var text = `line one
line "two"`
    println(text)
    println(`` == "")
}
