// test: a Drop that starts a task while the task holding the value (its result) is torn down on a worker
// status: passing
// run-flags: --leak-check
namespace spec.async.drop_starts_task

import std
import std.io

struct Guard {
    wg *task.WaitGroup
    func Drop() {
        var wg = this.wg
        task.Go(async func() {
            wg.Done()
        })
    }
}

// The task's result is the last reference to the guard: it is freed when
// the last job running as this task (often the Yield job) finishes.
async func make(wg *task.WaitGroup) *Guard {
    await task.Yield()
    return new Guard{wg: wg}
}

async func main() {
    var wg = new task.WaitGroup{}
    wg.Add(300)
    for _ in 0..300 {
        _ = make(wg)
    }
    await wg.Wait()
    println("all 300 destructors started their tasks")
}
