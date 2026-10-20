// test: time.Format and calendar accessors on a fixed UTC timestamp (Unix ms)
// status: passing
namespace spec.stdlib.time_format_fixed

import std.io
import std

const T = 1700000000123

func main() {
    println(time.Format(T, "%Y-%m-%d %H:%M:%S.%L"))
    println(time.Format(T, "%A, %d %B %Y"))
    println(time.Year(T), time.Month(T), time.Day(T), time.Hour(T), time.Minute(T), time.Second(T), time.Weekday(T))
    println(time.Format(0, "%Y-%m-%dT%H:%M:%S"), time.Weekday(0))
    println(time.Format(-1, "%Y-%m-%d %H:%M:%S.%L"))
}
