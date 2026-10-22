// test: defers in an async func run when its body finishes, after its awaits
// status: passing
namespace spec.control.defer_in_async

import std
import std.io

async func Job(name string) int {
    defer println("cleanup", name)
    println("start", name)
    await task.Delay(1)
    println("resumed", name)
    return len(name)
}

async func main() {
    defer println("main done")
    var n = await Job("alpha")
    println("got", n)
    var m = await Job("be")
    println("got", m)
}
