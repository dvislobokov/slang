namespace bench.gzip

import std
import std.io

// gzip level 6: compress then decompress a generated 20 MB text (same text as main.go).
func main() void! {
    var words = [
        "lorem",
        "ipsum",
        "dolor",
        "sit",
        "amet",
        "consectetur",
        "adipiscing",
        "elit",
        "sed",
        "do",
        "eiusmod",
        "tempor",
        "incididunt",
        "ut",
        "labore",
        "et",
        "dolore",
        "magna",
        "aliqua",
        "1234567",
    ]
    var b = strings.Builder{}
    var seed = 12345
    var n = 0
    for n < 20000000 {
        seed = (seed * 1103515245 + 12345) % 2147483648
        var w = words[(seed >> 8) % 20]
        b.Write(w)
        b.Write(" ")
        n += len(w) + 1
    }
    var data = strings.Bytes(b.String())
    var t0 = time.NowNanos()
    var c = compress.gzip.Compress(data, 6)
    var t1 = time.NowNanos()
    var d = compress.gzip.Decompress(c)?
    var t2 = time.NowNanos()
    println(len(data), len(c), len(d) == len(data))
    io.Eprintln(
        fmt.Format("compress {0} ms, decompress {1} ms", (t1 - t0) / 1000000, (t2 - t1) / 1000000),
    )
}
