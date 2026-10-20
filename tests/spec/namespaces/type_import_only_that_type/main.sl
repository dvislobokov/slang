// error: unknown identifier Count
// status: passing
namespace spec.ns.typeonly

import std.io
import spec.ns.typeonly.models.User

func main() {
    var u = User{Name: "x"}
    println(u.Name, Count())
}
