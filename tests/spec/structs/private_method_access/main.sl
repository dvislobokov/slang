// error: private
// status: passing
namespace spec.structs.privm

import std.io

import spec.structs.privm.lib

func main() {
    var s = Service{}
    s.reset()
    println("x")
}
