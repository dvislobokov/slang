// test: indexing past the end of a string panics with exit code 2
// status: passing
// exit: 2
namespace spec.basics.string_index_out_of_range

import std.io

func main() {
    var s = "abc"
    var i = 3
    println("before")
    println(s[i])
}
