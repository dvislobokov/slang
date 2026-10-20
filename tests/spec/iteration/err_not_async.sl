// error: awaits std.task.Channel<int>.Next, which is async; main is not declared async
// status: passing
namespace spec.iteration.err_not_async

import std.io
import std

func main() {
    var ch = task.Channel<int>(1)
    ch.Close()
    for v in ch {
        println(v)
    }
}
