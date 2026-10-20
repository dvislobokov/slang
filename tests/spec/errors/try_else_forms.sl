// test: try ... else with a default value, continue and break
// status: passing
namespace spec.errors.try_else_forms

import std.io
import std

func main() {
    var a = try strings.ToInt("12") else -1
    var b = try strings.ToInt("x") else -1
    println(a, b)
    var sum = 0
    for s in ["1", "two", "3", "stop", "100"] {
        if s == "stop" {
            var _x = try strings.ToInt(s) else break
            println("unreachable", _x)
        }
        var n = try strings.ToInt(s) else continue
        sum += n
    }
    println(sum)
    var c = (try strings.ToInt("5") else 0) * 2 + (try strings.ToInt("?") else 1)
    println(c)
}
