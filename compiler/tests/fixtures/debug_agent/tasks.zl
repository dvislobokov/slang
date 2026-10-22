namespace app

import std
import std.io

// Debug-agent fixture: two tasks running the same code, stepped
// independently.

async func worker(n int) int {
    var acc = 0
    await task.Delay(5)
    acc += n // BREAK w1
    acc += n * 2 // BREAK w2
    acc += n * 3 // BREAK w3
    return acc
}

async func main() {
    var a = worker(1)
    var b = worker(10)
    var ra = await a
    var rb = await b
    println(ra, rb)
}
