// error: cannot be stored as a map with values of
// status: passing
namespace spec.diagnostics.drop_in_map_value

import std.io

struct R {
    func Drop() {}
}

func main() {
    var m = map[string]R{}
    println(len(m))
}
