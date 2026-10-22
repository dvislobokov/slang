// error: default methods of interfaces cannot be static, async or generic
// status: passing
namespace spec.diagnostics.default_method_async

import std.io

interface Loader {
    func Load() string
    async func Prefetch() { this.Load() }
}

func main() {
    println("unreachable")
}
