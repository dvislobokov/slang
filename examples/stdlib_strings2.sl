namespace examples.stdlib_strings2

import std
import std.io

func main() {
    // More std.strings.
    println("fields:", strings.Fields("  the quick\tbrown  fox \n"), len(strings.Fields("   ")))
    println("count:", strings.Count("cheese", "e"), strings.Count("five", ""))
    println("last index:", strings.LastIndex("go gopher", "go"), strings.LastIndex("x", "y"))
    println(
        "trim:",
        strings.TrimPrefix("v1.2.3", "v"),
        strings.TrimSuffix("main.sl", ".sl"),
        strings.TrimPrefix("abc", "x"),
    )
    println(
        "cutset: [" +
            strings.TrimLeft("xxhixx", "x") +
            "] [" +
            strings.TrimRight("xxhixx", "x") +
            "] [" +
            strings.TrimLeft("¡¡hola!!", "¡!") +
            "]",
    )
    println(
        "splitn:",
        strings.SplitN("a=b=c", "=", 2),
        len(strings.SplitN("a,b", ",", 0)),
        strings.SplitN("a,b,c", ",", -1),
    )
    println(
        "fold:",
        strings.EqualFold("Slang", "SLANG"),
        strings.EqualFold("Привет", "пРИВЕТ"),
        strings.EqualFold("go", "goo"),
    )
    println("reverse:", strings.Reverse("héllo, 世界"))
    println(
        "pad: [" +
            strings.PadLeft("42", 6, "0") +
            "] [" +
            strings.PadRight("ab", 5, ".") +
            "] [" +
            strings.PadLeft("toolong", 3, "*") +
            "]",
    )
    println(
        "compare:",
        strings.Compare("a", "b"),
        strings.Compare("b", "a"),
        strings.Compare("x", "x"),
    )
    println("title:", strings.Title("hello wide world"))

    var runes = strings.Runes("añ€")
    println("runes:", len(runes), runes)
    runes[0] = 'A'
    println("from runes:", strings.FromRunes(runes))
    var bytes = strings.Bytes("Hi")
    println("bytes:", bytes)
    bytes = append(bytes, 33)
    println("from bytes:", strings.FromBytes(bytes))

    // More std.math: float functions, then the int helpers IMin/IMax/IAbs.
    println("round:", math.Round(2.5), math.Round(-2.5), math.Trunc(-3.7), math.Mod(7.5, 2.0))
    println("hypot:", math.Hypot(3.0, 4.0), "log2:", math.Log2(1024.0), "log10:", math.Log10(0.001))
    println("atan2:", math.Atan2(1.0, 1.0) * 4.0)
    println(
        "nan/inf:",
        math.IsNaN(math.NaN()),
        math.IsInf(math.Inf(1), 1),
        math.IsInf(math.Inf(-1), 1),
        math.IsInf(2.0, 0),
    )
    println("ints:", math.IMin(3, -7), math.IMax(3, -7), math.IAbs(-12))
}
