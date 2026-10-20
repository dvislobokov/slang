// test: two namespaces of one program may refer to each other (no import cycles restriction)
// status: passing
namespace spec.ns.mutual

import std.io
import spec.ns.mutual.a

func main() {
    println(Ping(3))
}
