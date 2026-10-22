// test: std.regex named groups (?P<name>) and (?<name>), Groups API (At, Get, Span, Named, Matched, Names, Len, Match), GroupNames/GroupIndex/NumGroups, GroupsAll, ReplaceGroupsFunc, Iter, FindIndex, FindAllN, FindAllText, MatchBytes, String
// status: passing
namespace spec.stdlib3.regex_groups

import std
import std.io

func main() void! {
    var date = regex.Pattern(`(?P<year>\d{4})-(?P<month>\d{2})-(?<day>\d{2})(T(\d\d):(\d\d))?`)
    println(date.String())
    println(date.NumGroups(), date.GroupNames(), date.GroupIndex("day"), date.GroupIndex("nope"))
    var g = date.Groups("released 2024-05-17 and 2025-01-02T10:30")
    println(g.Len(), g.At(0), g.Get("year"), g.Get("month"), g.Get("day"))
    println(g.Matched(4), g.At(4) == "", g.Span(4) == nil, g.Names())
    var whole = g.Match()
    println(whole.Start, whole.End, whole.Text)
    var y = g.Named("year")
    println(y.Start, y.End, y.Text, g.Get("unknown") == "", g.At(99) == "")
    for gg in date.GroupsAll("released 2024-05-17 and 2025-01-02T10:30") {
        println(gg.At(0), gg.Matched(4), gg.At(5), gg.At(6))
    }
    var swapped = date.ReplaceGroupsFunc(
        "on 2024-05-17.",
        (x *regex.Groups) => x.Get("day") + "/" + x.Get("month") + "/" + x.Get("year"),
    )
    println(swapped)

    var word = regex.Compile(`\w+`)?
    var it = word.Iter("one two  three")
    for {
        var m = it.Next()
        if m == nil {
            break
        }
        println(m.Start, m.End, m.Text)
    }
    println(it.Next() == nil)
    println(word.FindIndex("  hi"), word.FindIndex("!!") == nil, word.Find("!!") == nil)
    var two = word.FindAllN("a b c d", 2)
    println(len(two), two[1].Text)
    println(word.FindAllText("x, y; z"))
    println(word.MatchBytes(strings.Bytes("...a")), word.MatchBytes(strings.Bytes("...")))
    println(word.Groups("--") == nil, word.NumGroups())

    // a copy shares the compiled program
    var copy = word
    println(copy.Match("q"), copy.String())
}
