// test: Contains, Index, LastIndex, HasPrefix, HasSuffix, Count (byte offsets, -1 when absent)
// status: passing
namespace spec.stdlib.strings_search

import std.io
import std

func main() {
    println(strings.Contains("seafood", "foo"), strings.Contains("seafood", "bar"), strings.Contains("x", ""))
    println(strings.Index("chicken", "ken"), strings.Index("chicken", "dmr"), strings.Index("héllo", "l"), strings.Index("abc", ""))
    println(strings.LastIndex("go gopher", "go"), strings.LastIndex("go", "x"))
    println(strings.HasPrefix("golang", "go"), strings.HasPrefix("go", "golang"), strings.HasSuffix("main.sl", ".sl"), strings.HasSuffix("x", ""))
    println(strings.Count("cheese", "e"), strings.Count("aaaa", "aa"), strings.Count("five", ""))
}
