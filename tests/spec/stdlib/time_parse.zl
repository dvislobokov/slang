// test: time.Parse round-trips with Format and rejects mismatching input
// status: passing
namespace spec.stdlib.time_parse

import std
import std.io

func main() {
    var t = try time.Parse("2024-02-29 12:30:00", "%Y-%m-%d %H:%M:%S") else -1
    println(t, time.Format(t, "%d.%m.%Y %H:%M"))
    println(try time.Parse("14/11/2023", "%Y-%m-%d") else -1)
}
