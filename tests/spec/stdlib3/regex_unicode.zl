// test: std.regex on UTF-8 text: offsets are bytes, . and classes match whole code points, \p{L}/\p{Greek}/\p{Han}/\pN, negated Unicode classes, (?i) with Unicode case folding (K, σ/ς/Σ), \x{...}, ranges beyond ASCII, Split on Unicode separators
// status: passing
namespace spec.stdlib3.regex_unicode

import std
import std.io

func main() {
    var s = "Ünïcödé 世界 αβγ ΑΒΓ 123 ١٢٣"
    println(regex.Pattern(`\p{L}+`).FindAllText(s))
    println(regex.Pattern(`\p{Greek}+`).FindAllText(s))
    println(regex.Pattern(`\p{Han}+`).FindAllText(s))
    println(regex.Pattern(`\pN+`).FindAllText(s))
    println(regex.Pattern(`\p{Lu}`).FindAllText(s))
    println(regex.Pattern(`[^\p{L}\s]+`).FindAllText(s))
    var m = regex.Pattern(`世界`).Find(s)
    println(m.Start, m.End, len(m.Text))
    println(regex.Pattern(`^.{7}`).Find(s).Text)
    println(regex.Pattern(`(?i)αβγ`).FindAllText(s))
    println(regex.Pattern(`(?i)σ`).FindAllText("ΣσςS"))
    println(regex.Pattern(`(?i)k`).FindAllText("kK\u{212A}x"))
    println(regex.Pattern(`[\x{4e00}-\x{9fff}]+`).FindAllText(s))
    println(regex.Pattern(`\x{263a}`).Match("smile ☺"))
    println(regex.Pattern(`[à-ÿ]+`).FindAllText("voilà déjà ça"))
    println(regex.Pattern(`\s*·\s*`).Split("a · b·c"))
    println(regex.Pattern(``).Split("日本"))
    println(regex.Pattern(`\b\w+\b`).FindAllText("über straße"))
}
