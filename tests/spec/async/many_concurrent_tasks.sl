// test: many tasks started before awaiting any; results gathered in start order
// status: passing
namespace spec.async.many_concurrent_tasks

import std.io
import std

async func Square(n int) int {
    await task.Delay(n % 3)
    return n * n
}

async func main() {
    var tasks []Task<int> = []
    for i in 0..50 {
        tasks = append(tasks, Square(i))
    }
    var sum = 0
    for t in tasks {
        sum += await t
    }
    println(sum)
}
