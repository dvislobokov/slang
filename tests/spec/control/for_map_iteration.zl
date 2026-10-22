// test: for k in m yields keys, for k, v in m yields both (order-independent use)
// status: passing
namespace spec.control.for_map_iteration

import std.io

func main() {
    var m = map[string]int{"a": 1, "b": 20, "c": 300}
    var keys = 0
    for k in m {
        keys += len(k)
    }
    var sum = 0
    for k, v in m {
        if k != "b" {
            sum += v
        }
    }
    var vals = 0
    for _, v in m {
        vals += v
    }
    println(keys, sum, vals)
}
