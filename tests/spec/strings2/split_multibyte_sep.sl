// test: Split, Index, Replace and Count with multibyte separators work on bytes of whole code points
// status: passing
namespace spec.strings2.split_multibyte_sep

import std.io
import std

func main() {
    var parts = strings.Split("a→bb→→c", "→")
    println(len(parts), parts)
    println(strings.Split("東京と大阪と", "と"))
    println(strings.Index("日本語", "語"), strings.Index("日本語", "x"))
    println(strings.Replace("ça va, ça va", "ça", "it"))
    println(strings.Count("ёёё", "ё"), strings.Join(["я", "ты"], "·"))
}
