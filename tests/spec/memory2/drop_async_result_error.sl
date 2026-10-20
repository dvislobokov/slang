// error: cannot return a value with a destructor
// status: passing
namespace spec.memory2.drop_async_result_error

import std.io

struct R {
    Name string
    func Drop() { println("drop") }
}

async func Make() R {
    return R{Name: "x"}
}

async func main() {
    var r = await Make()
    println(r.Name)
}
