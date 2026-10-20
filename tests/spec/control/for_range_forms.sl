// test: half-open integer ranges with literal, variable, empty and negative bounds
// status: passing
namespace spec.control.for_range_forms

import std
import std.io

func main() {
    var out = ""
    for i in 0..3 {
        out += fmt.Format("{0} ", i)
    }
    println(out)
    var lo = -2
    var hi = 2
    out = ""
    for i in lo..hi {
        out += fmt.Format("{0} ", i)
    }
    println(out)
    var n = 0
    for i in 5..2 {
        n += i
    }
    for i in 3..3 {
        n += i
    }
    println("empty ranges ran", n, "times")
}
