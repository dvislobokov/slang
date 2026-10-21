// test: await may appear in switch subjects, case bodies and guards of an async function
// status: passing
namespace spec.async2.await_in_switch_arms

import std
import std.io

async func Double(n int) int {
    await task.Delay(1)
    return n * 2
}

enum Op {
    Inc(n int)
    Dbl(n int)
    Nop
}

async func Apply(op Op) int {
    switch op {
        case Op.Inc(n): return n + 1
        case Op.Dbl(n): return await Double(n)
        default:
            return 0
    }
}

async func main() {
    switch await Double(2) {
        case 4: println("four")
        default:
            println("other")
    }
    var ops = [Op.Inc(1), Op.Dbl(5), Op.Nop]
    for op in ops {
        println(await Apply(op))
    }
    var x = 3
    switch {
        case await Double(x) > 5: println("big", await Double(x))
        default:
            println("small")
    }
}
