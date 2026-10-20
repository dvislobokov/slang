namespace examples.strings

import std.io
import std

func main() {
    var greeting = "Hello"
    var name = "Slang"
    var msg = greeting + ", " + name + "!"
    println(msg)
    println("len:", len(msg))

    // Comparison is byte-wise lexicographic.
    println("equal:", "abc" == "abc", "less:", "apple" < "banana", "greater:", "b" > "abc")

    // std.strings
    var parts = strings.Split("red,green,blue", ",")
    println("parts:", len(parts), parts[0], parts[2])
    println("joined:", strings.Join(parts, " | "))
    println("[" + strings.Trim("   padded   ") + "]")
    println("contains:", strings.Contains(msg, "Slang"), strings.Contains(msg, "Go"))
    println("upper:", strings.ToUpper(msg))
    println("replace:", strings.Replace("a-b-c", "-", "+"))

    var n = try strings.ToInt("42") else 0
    var neg = try strings.ToInt("-17") else 0
    var bad = try strings.ToInt("4x2") else -1
    println("ToInt:", n + 1, neg, bad)

    // Iterating a string yields chars (Unicode code points).
    var vowels = 0
    for ch in "programming language" {
        switch ch {
            case 'a', 'e', 'i', 'o', 'u':
                vowels += 1
        }
    }
    println("vowels:", vowels)

    var word = "héllo"
    var chars = 0
    for _ in word {
        chars += 1
    }
    println("bytes:", len(word), "chars:", chars)

    for ch in "abc" {
        println("char:", ch)
    }

    var reversed = ""
    for ch in "stressed" {
        reversed = string(ch) + reversed
    }
    println("reversed:", reversed)

    // std.fmt
    println(fmt.Format("{0} is {1} years old", "Alice", 31))
    println(fmt.Format("{1} before {0}, {0} again", "x", "y"))
    println(fmt.Format("pi ~ {0}, ok = {1}", 3.14, true))

    var csv = ""
    for i, p in parts {
        if i > 0 {
            csv += ";"
        }
        csv += strings.ToUpper(p)
    }
    println("csv:", csv)

    // Escapes.
    println("tab:\tend")
    println("quote: \"q\" backslash: \\")
}
