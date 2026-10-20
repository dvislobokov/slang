// error: are not supported (only functions, methods, structs and enums can be generic)
// status: passing
namespace spec.diagnostics.generic_interface

import std.io

interface Getter<T> {
    func Get() T
}

func main() {
    println("x")
}
