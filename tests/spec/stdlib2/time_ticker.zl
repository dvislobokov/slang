// test: std.time async Ticker (fixed cadence, Stop), Timer (Wait, Stop, Reset) and After
// status: passing
namespace spec.stdlib2.time_ticker

import std
import std.io

async func main() {
    var start = time.NowInstant()
    var t = time.NewTicker(time.Milliseconds(10))
    var n = 0
    for await t.Tick() {
        n += 1
        if n == 3 {
            t.Stop()
        }
    }
    var el = start.Elapsed().Milliseconds()
    println("ticks:", n, el >= 30, el < 1000)
    var timer = time.NewTimer(time.Milliseconds(5))
    println("fired:", await timer.Wait(), timer.Stop())
    var t2 = time.NewTimer(time.Seconds(10))
    println("stopped pending:", t2.Stop(), await t2.Wait())
    _ = t2.Reset(time.Milliseconds(1))
    println("after reset:", await t2.Wait())
    var at = await time.After(time.Milliseconds(2))
    println("after:", !at.IsZero())
}
