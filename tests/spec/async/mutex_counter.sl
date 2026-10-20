// test: Mutex serialises increments from concurrent tasks
// status: pending
namespace spec.async.mutex_counter

import std.io
import std

struct Shared {
    Mu task.Mutex
    N int
}

async func Bump(s *Shared, times int) {
    for _ in 0..times {
        await s.Mu.Lock()
        s.N += 1
        s.Mu.Unlock()
        await task.Delay(0)
    }
}

async func main() {
    var s = new Shared{}
    await task.All(Bump(s, 100), Bump(s, 100), Bump(s, 100))
    println(s.N)
}
