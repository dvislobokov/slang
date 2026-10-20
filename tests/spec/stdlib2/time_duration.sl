// test: std.time.Duration constructors, accessors, Go-style String, ParseDuration, Truncate/Round
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.time_duration

import std
import std.io

func main() {
    var d = time.Hours(1).Add(time.Minutes(2)).Add(time.Milliseconds(3500))
    println(d.String(), d.Nanoseconds(), d.Milliseconds(), d.Seconds(), d.Minutes(), d.Hours())
    for x in [time.Duration{}, time.Nanoseconds(1), time.Microseconds(1500), time.Milliseconds(300), time.Seconds(90), time.Hours(26).Neg(), time.SecondsF(1.25), time.Nanoseconds(1001)] {
        print(x.String(), " ")
    }
    println()
    for s in ["300ms", "-1.5h", "2h45m", "1h2m3.5s", "10us", "7µs", "0", "+5s", "1.5", "3x", ""] {
        var r = try time.ParseDuration(s) catch err {
            println(err)
            continue
        }
        println(s, "=", r.String(), r.Nanoseconds())
    }
    var x = time.Milliseconds(1750)
    println(x.Truncate(time.Seconds(1)).String(), x.Round(time.Seconds(1)).String(), x.Neg().Round(time.Seconds(1)).String(), x.Round(time.Duration{}).String())
    println(x.Mul(2).String(), x.Div(7).String(), x.Sub(time.Seconds(2)).Abs().String(), x.Compare(time.Seconds(1)), x.Equals(time.Microseconds(1750000)))
    var i = time.NowInstant()
    time.SleepFor(time.Milliseconds(5))
    println(i.Elapsed().Milliseconds() >= 5, time.NowInstant().Sub(i).Nanoseconds() > 0)
}
