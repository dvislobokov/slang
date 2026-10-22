// test: len counts bytes, strings.Runes counts code points (ASCII, Latin-1, Cyrillic, CJK, emoji)
// status: passing
namespace spec.strings2.len_bytes_vs_runes

import std
import std.io

func main() {
    for s in ["abc", "héllo", "привет", "日本語", "🙂👍", ""] {
        println(len(s), len(strings.Runes(s)))
    }
}
