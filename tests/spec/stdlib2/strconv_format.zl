// test: std.strconv FormatInt/FormatFloat/ParseFloat/ParseBool/Quote/Unquote and generic Parse<T>/Format<T>
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.strconv_format

import std
import std.io

func main() {
    println(
        strconv.FormatInt(-255, 16),
        strconv.FormatInt(math.MinInt, 10),
        strconv.FormatUint(18446744073709551615, 36),
        strconv.Itoa(0),
        strconv.FormatInt(5, 2),
    )
    println(
        strconv.FormatFloat(3.14159, 'f', 2),
        strconv.FormatFloat(1234.5678, 'e', 3),
        strconv.FormatFloat(0.000012, 'g', -1),
        strconv.FormatFloat(1e6, 'g', -1),
    )
    println(
        strconv.FormatFloat(0.1, 'f', -1),
        strconv.FormatFloat(123456789.0, 'f', -1),
        strconv.FormatFloat(2.5, 'E', -1),
        strconv.FormatFloat(100, 'g', 5),
        strconv.FormatFloat(-0.0, 'g', -1),
    )
    println(
        strconv.FormatFloat(math.Inf(1), 'f', 2),
        strconv.FormatFloat(math.NaN(), 'g', -1),
        strconv.FormatFloat(1.0 / 3.0, 'g', -1),
    )
    println(
        try strconv.ParseFloat("1.5e3") else -1,
        try strconv.ParseFloat("-0x1p-2") else -1,
        try strconv.ParseFloat("inf") else -1,
        try strconv.ParseFloat("1.5x") else -1,
    )
    try {
        _ = strconv.ParseFloat("1e400")?
    } catch err {
        println(err, errors.Is(err, strconv.ErrRange))
    }
    println(
        try strconv.ParseBool("True") else false,
        try strconv.ParseBool("0") else true,
        try strconv.ParseBool("yes") else false,
        strconv.FormatBool(true),
    )
    println(
        strconv.Quote("hi \"there\"\n\ttab é \u{1F600}"),
        strconv.QuoteToASCII("é\u{1F600}"),
        strconv.QuoteRune('\''),
        strconv.QuoteRune('x'),
    )
    println(strconv.Quote(strings.FromBytes([byte(104), 255, 1])))
    println(
        try strconv.Unquote("\"a\\tb\\x41\\u00e9\\U0001F600\\101\\u{48}\"") else "?",
        try strconv.Unquote("`raw\\n`") else "?",
        try strconv.Unquote("'x'") else "?",
    )
    println(
        try strconv.Unquote("\"bad") else "bad1",
        try strconv.Unquote("'ab'") else "bad2",
        try strconv.Unquote("\"\\q\"") else "bad3",
    )
    var rt = strconv.Quote("line\nnext \\ \"q\"")
    println(try strconv.Unquote(rt) else "?")
    var i = try strconv.Parse<int>("42") else 0
    var b = try strconv.Parse<byte>("255") else 0
    var f = try strconv.Parse<float>("2.5") else 0
    var ok = try strconv.Parse<bool>("true") else false
    var s = try strconv.Parse<string>("text") else ""
    var c = try strconv.Parse<char>("é") else 'x'
    println(
        i,
        b,
        f,
        ok,
        s,
        c,
        try strconv.Parse<int8>("200") else -1,
        try strconv.Parse<uint16>("70000") else 1,
    )
    println(
        strconv.Format(42),
        strconv.Format(2.5),
        strconv.Format(true),
        strconv.Format("s"),
        strconv.Format(byte(7)),
        strconv.Format('z'),
        strconv.Format(int32(-3)),
    )
}
