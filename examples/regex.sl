namespace examples.regex

import std
import std.io

// std.regex: RE2 syntax, linear time, literal patterns checked by the compiler.
func main() void! {
    var lines = [
        "2024-05-17 12:00:01 INFO  user=alice action=login ip=10.0.0.7",
        "2024-05-17 12:00:09 WARN  user=bob action=retry ip=10.0.0.9",
        "2024-05-17 12:01:30 ERROR user=alice action=pay ip=10.0.0.7",
    ]
    // Pattern: the literal is validated at compile time (a typo is a build error).
    var entry = regex.Pattern(`^(?P<date>\d{4}-\d\d-\d\d) (?P<time>[\d:]+) (?P<level>[A-Z]+)\s+(?P<rest>.*)$`)
    var kv = regex.Pattern(`(\w+)=(\S+)`)
    for line in lines {
        var g = entry.Groups(line)
        if g == nil {
            continue
        }
        var fields = kv.GroupsAll(g.Get("rest"))
        println(g.Get("level"), g.Get("time"), len(fields), "fields, user:", fields[0].At(2))
    }

    // Replace with group references; Split; Escape for literal text.
    println(kv.Replace("user=alice ip=10.0.0.7", "$1:<$2>"))
    println(regex.Pattern(`\s*[,;]\s*`).Split("a , b;c ,d"))
    var dots = regex.Compile(regex.Escape("1.5+2"))?
    println(dots.String(), dots.Match("x1.5+2y"), dots.Match("1x5+2"))

    // Compile reports errors with an offset; dynamic patterns use it.
    try regex.Compile("(unclosed") catch err {
        println(err.Message())
    }

    // Linear time: no catastrophic backtracking.
    var evil = regex.Pattern(`(a+)+$`)
    println(evil.Match(strings.Repeat("a", 20000) + "!"))
}
