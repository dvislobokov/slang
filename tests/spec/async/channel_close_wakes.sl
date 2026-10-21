// test: closing a Channel wakes every waiting receiver with an error; values buffered before Close are still delivered
// status: passing
// run-flags: --leak-check
namespace spec.async.channel_close_wakes

import std
import std.io

async func Wait(ch task.Channel<int>) string {
    var v = try await ch.Recv() catch err {
        return "error: " + err.Message()
    }
    return fmt.Format("value {0}", v)
}

async func main() {
    var ch = task.Channel<int>(1)
    var a = Wait(ch)
    var b = Wait(ch)
    await task.Delay(5)
    await ch.Send(7)
    await task.Delay(5)
    ch.Close()
    var ra = await a
    var rb = await b
    if ra > rb {
        println(rb, "|", ra)
    } else {
        println(ra, "|", rb)
    }
    var late = task.Channel<int>(2)
    await late.Send(1)
    late.Close()
    println(await Wait(late), await Wait(late))
}
