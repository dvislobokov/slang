// error: invalid map key type
// status: passing
namespace spec.diagnostics.invalid_map_key

import std.io

func main() {
    var m = map[[]int]string{}
    println(len(m))
}
