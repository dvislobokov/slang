// error: cannot keep an interior pointer
// status: passing
namespace spec.memory2.interior_pointer_return_error

import std.io

struct Box {
    V int
    func Ptr() *int {
        return &this.V
    }
}

func main() {
    var b = new Box{V: 1}
    println(*b.Ptr())
}
