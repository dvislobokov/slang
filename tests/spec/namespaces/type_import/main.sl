// test: import app.models.User imports a single type by its short name
// status: passing
namespace spec.ns.typeimp

import std.io

import spec.ns.typeimp.models.User

func main() {
    var u = User.New("ann")
    println(u.Name, spec.ns.typeimp.models.Count())
}
