// test: var without initializer gets the zero value of its type
// status: passing
namespace spec.basics.zero_values

import std.io

struct P {
    X int
    Name string
}

func main() {
    var i int
    var f float
    var b bool
    var s string
    var c char
    var sl []int
    var m map[string]int
    var p *P
    var v P
    var arr [2]bool
    println(i, f, b, "[" + s + "]", int(c), len(sl), len(m), p == nil)
    println(v.X, "[" + v.Name + "]", arr, sl)
}
