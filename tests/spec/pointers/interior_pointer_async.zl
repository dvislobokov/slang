// test: an interior pointer variable in an async function keeps its slice alive across awaits and reassignment
// status: passing
// run-flags: --leak-check
namespace spec.pointers.interior_pointer_async

import std
import std.io

struct P {
    X int
}

func peek(p *P) int {
    return p.X
}

async func work(xs []P) int {
    var p = &xs[0]
    await task.Yield()
    xs = [P{X: 5}]
    await task.Delay(1)
    p.X += 1
    return peek(p) + xs[0].X
}

async func main() {
    println(await work([P{X: 1}]))
    var ps = [P{X: 1}]
    var t = &ps[0]
    await task.Yield()
    ps = nil
    println(t.X)
}
