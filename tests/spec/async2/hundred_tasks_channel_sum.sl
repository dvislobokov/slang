// test: 100 concurrent tasks send their results to one channel; the receiver sums exactly 100 values
// status: passing
namespace spec.async2.hundred_tasks_channel_sum

import std.io
import std

async func Worker(id int, out task.Channel<int>) {
    await task.Delay(id % 7)
    await out.Send(id * 2)
}

async func main() {
    var ch = task.Channel<int>(10)
    var tasks []Task<void> = []
    for i in 1..101 {
        tasks = append(tasks, Worker(i, ch))
    }
    var sum = 0
    for _ in 0..100 {
        sum += try await ch.Recv() else 0
    }
    for t in tasks {
        await t
    }
    println("sum", sum)
}
