// test: std.time.ParseTime / ParseInLocation with Go layouts, zone offsets, fractions and errors
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.time_parse

import std
import std.io

func show(layout string, value string) {
    var t = try time.ParseTime(layout, value) catch err {
        println("error:", err)
        return
    }
    println(t.Format(time.RFC3339Nano), t.Unix())
}

func main() {
    show(time.RFC3339, "2024-02-29T13:04:05Z")
    show(time.RFC3339, "2024-02-29T13:04:05.5+02:00")
    show(time.RFC3339Nano, "2024-02-29T13:04:05.123456789-07:30")
    show(time.RFC1123, "Thu, 29 Feb 2024 13:04:05 GMT")
    show(time.RFC1123Z, "Thu, 29 Feb 2024 13:04:05 +0100")
    show(time.DateOnly, "1999-12-31")
    show(time.Kitchen, "3:04PM")
    show("02/01/06 3:04pm", "29/02/24 12:30am")
    show("Jan _2 2006", "Feb  9 2024")
    show("2006-002", "2024-060")
    show("2006-01-02 15:04:05.000", "2024-01-02 03:04:05.678")
    show(time.DateTime, "2024-01-02 03:04:05.25")
    show(time.DateOnly, "2024-02-30")
    show(time.DateOnly, "2024-1-02")
    show(time.DateOnly, "2024-01-02x")
    show(time.RFC3339, "2024-01-02T03:04:05")
    var ny = try time.LoadLocation("America/New_York") else time.UTC()
    var t = try time.ParseInLocation(time.DateTime, "2024-07-04 12:00:00", ny) else time.Time{}
    println(t.String(), t.UTC().Format(time.RFC3339))
    var mst = try time.ParseTime(time.UnixDate, "Thu Feb 29 13:04:05 PST 2024") else time.Time{}
    var (n, o) = mst.Zone()
    println(mst.Format(time.UnixDate), n, o)
    var now = time.Current()
    var t2 = try time.ParseTime(time.RFC3339Nano, now.UTC().Format(time.RFC3339Nano)) else time.Time{}
    println(t2.Equal(now), time.Time.Now().After(now) || time.Time.Now().Equal(now), now.Location().String(), time.Elapsed(now).Nanoseconds() >= 0, time.Until(now).Nanoseconds() <= 0)
}
