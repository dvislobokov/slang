// test: a defer runs after the return expression is evaluated
// status: passing
namespace spec.control.defer_modifies_after_return_value

import std.io

var trace = ""

func Get() string {
    defer func() {
        trace += "defer;"
    }()
    trace += "body;"
    return Mark()
}

func Mark() string {
    trace += "return-expr;"
    return "value"
}

func main() {
    var v = Get()
    println(v, trace)
}
