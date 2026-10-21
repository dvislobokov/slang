// Examples for std.strings (run by `slang test std`, shown by `slang doc`).
namespace strings_test

import std
import std.io

func ExampleSplit() {
    var parts = strings.Split("a,b,c", ",")
    println(len(parts), parts)
    // Output:
    // 3 [a b c]
}

func ExampleSplit_empty() {
    // An empty separator splits after each UTF-8 sequence.
    println(strings.Split("héllo", ""))
    // Output:
    // [h é l l o]
}

func ExampleJoin() {
    println(strings.Join(["usr", "local", "bin"], "/"))
    // Output:
    // usr/local/bin
}

func ExampleContains() {
    println(strings.Contains("seafood", "foo"), strings.Contains("seafood", "bar"))
    // Output:
    // true false
}

func ExampleReplace() {
    println(strings.Replace("oink oink oink", "k", "ky"))
    // Output:
    // oinky oinky oinky
}

func ExampleTrim() {
    println("[" + strings.Trim("  \t hello \n") + "]")
    // Output:
    // [hello]
}

func ExampleFields() {
    println(strings.Fields("  foo bar   baz  "))
    // Output:
    // [foo bar baz]
}

func ExampleHasPrefix() {
    println(strings.HasPrefix("slang.toml", "slang"), strings.HasSuffix("slang.toml", ".json"))
    // Output:
    // true false
}

func ExampleIndex() {
    println(strings.Index("chicken", "ken"), strings.Index("chicken", "dmr"))
    // Output:
    // 4 -1
}

func ExampleToUpper() {
    println(strings.ToUpper("Gopher"), strings.ToLower("SLANG"))
    // Output:
    // GOPHER slang
}

func ExampleToInt() {
    var n = try strings.ToInt("-42") else 0
    var bad = try strings.ToInt("4x2") else -1
    println(n, bad)
    // Output:
    // -42 -1
}

func ExampleRepeat() {
    println(strings.Repeat("ab", 3))
    // Output:
    // ababab
}

func ExampleBuilder() {
    var b = strings.Builder{}
    for i in 0..3 {
        b.Write(fmt.Format("{0};", i))
    }
    println(b.String(), b.Len())
    // Output:
    // 0;1;2; 6
}
