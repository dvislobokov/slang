// test: async functions awaiting each other 20 levels deep, with a delay at the bottom and an error path
// status: passing
namespace spec.async2.nested_async_20_deep

import std
import std.io

async func Level(n int) int {
    if n == 0 {
        await task.Delay(1)
        return 0
    }
    return 1 + await Level(n - 1)
}

async func LevelErr(n int) int! {
    if n == 0 {
        return error("bottom")
    }
    return 1 + await LevelErr(n - 1)?
}

async func main() {
    println(await Level(20))
    try await LevelErr(20) catch err {
        println(err, len(errors.Frames(err)))
    }
}
