// test: task.All over three tasks yields a tuple in argument order
// status: passing
namespace spec.async.task_all_values

import std
import std.io

async func Num(n int, ms int) int {
    await task.Delay(ms)
    return n
}

async func Word(s string) string {
    await task.Delay(5)
    return s
}

async func main() {
    var (x, y, z) = await task.All(Num(1, 20), Word("two"), Num(3, 1))
    println(x, y, z)
}
