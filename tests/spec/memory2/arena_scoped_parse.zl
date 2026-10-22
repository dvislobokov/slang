// test: one arena per request: objects allocated in it are valid until Free, then the next request starts a new arena
// status: passing
namespace spec.memory2.arena_scoped_parse

import std
import std.io

struct Token {
    Text string
}

func Handle(words []string) int {
    var a = arena.New()
    defer a.Free()
    var toks []*Token = []
    for w in words {
        var t = a.New<Token>()
        t.Text = w
        toks = append(toks, t)
    }
    var n = 0
    for t in toks {
        n += len(t.Text)
    }
    return n
}

func main() {
    println(Handle(["a", "bb", "ccc"]))
    println(Handle(["dddd"]))
}
