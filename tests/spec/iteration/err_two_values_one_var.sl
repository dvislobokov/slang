// error: takes two variables (key, value)
// status: passing
namespace spec.iteration.err_two_values_one_var

import std.io
import std.collections

func main() {
    var m = OrderedMap<string, int>{}
    m.Set("a", 1)
    for k in m {
        println(k)
    }
}
