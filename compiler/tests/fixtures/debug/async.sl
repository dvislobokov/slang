namespace app

import std
import std.io

// Debugger fixture: locals of async functions across `await`.

async func Square(n int) int {
    await task.Delay(1)
    return n * n
}

async func Half(n int) int! {
    await task.Delay(1)
    if n % 2 != 0 {
        return error("odd")
    }
    return n / 2
}

async func Work(label string, n int) int {
    var before = n + 1
    var sq = await Square(n) // BREAK before_await
    var after = sq + before // BREAK after_await
    println(label, after)
    return after
}

async func main() {
    var t = Half(3)
    var w = Work("work", 4)
    var r = await w
    var h = try await t else -1 // BREAK tasks
    println(r, h)
}
