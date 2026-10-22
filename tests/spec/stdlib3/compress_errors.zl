// test: std.compress reports corrupt input: bad gzip/zlib headers, checksum and size mismatches, truncated streams, invalid block types and distances, trailing garbage, zstd bad magic / truncated / corrupted / dictionary frames
// status: passing
namespace spec.stdlib3.compress_errors

import std
import std.io

func show(f func() []byte!) {
    var b = try f() catch err {
        println("error:", err.Message())
        return
    }
    println("ok", len(b))
}

func flip(b []byte, i int) []byte {
    var c []byte = []
    for x in b {
        c = append(c, x)
    }
    c[i] = c[i] ^ byte(255)
    return c
}

func cut(b []byte, n int) []byte {
    var c []byte = []
    for k in 0..n {
        c = append(c, b[k])
    }
    return c
}

func main() void! {
    var data = strings.Bytes(strings.Repeat("abc", 100))
    var g = compress.gzip.Compress(data, 6)
    show(func() []byte! {
        return compress.gzip.Decompress(g)?
    })
    show(func() []byte! {
        return compress.gzip.Decompress(strings.Bytes("not gzip at all"))?
    })
    show(func() []byte! {
        return compress.gzip.Decompress(flip(g, len(g) - 8))?
    })
    show(func() []byte! {
        return compress.gzip.Decompress(flip(g, len(g) - 2))?
    })
    show(func() []byte! {
        return compress.gzip.Decompress(cut(g, len(g) - 3))?
    })
    show(func() []byte! {
        return compress.gzip.Decompress(cut(g, 5))?
    })
    var z = compress.zlib.Compress(data, 6)
    show(func() []byte! {
        return compress.zlib.Decompress(flip(z, 0))?
    })
    show(func() []byte! {
        return compress.zlib.Decompress(flip(z, len(z) - 1))?
    })
    show(func() []byte! {
        return compress.zlib.Decompress(cut(z, len(z) - 1))?
    })
    show(func() []byte! {
        return compress.deflate.Decompress([byte(7)])?
    })
    show(func() []byte! {
        return compress.deflate.Decompress([byte(5), byte(0)])?
    })
    show(func() []byte! {
        return compress.deflate.Decompress([byte(1), byte(5), byte(0), byte(1), byte(1)])?
    })
    // a match at distance 4 in a stream holding 2 bytes
    show(func() []byte! {
        return compress.deflate.Decompress(encoding.hex.Decode("4b046200")?)?
    })
    var d = compress.deflate.Compress(data, 6)
    d = append(d, byte(0))
    show(func() []byte! {
        return compress.deflate.Decompress(d)?
    })
    show(func() []byte! {
        return compress.zstd.Decompress(strings.Bytes("zstd? no"))?
    })
    var zs = encoding.hex.Decode("28b52ffd200a5100006672616d652074776f0a")?
    show(func() []byte! {
        return compress.zstd.Decompress(zs)?
    })
    show(func() []byte! {
        return compress.zstd.Decompress(cut(zs, 12))?
    })
    show(func() []byte! {
        return compress.zstd.Decompress(
            encoding.hex.Decode("28b52ffd2101000a5100006672616d652074776f0a")?,
        )?
    })
    show(func() []byte! {
        return compress.zstd.Decompress(
            encoding.hex.Decode("28b52ffd240a510000aaaaaaaaaaaaaaaaaaaa00000000")?,
        )?
    })
    // streaming readers report errors when fed and when closed early
    var r = compress.gzip.NewReader()
    r.Feed(cut(g, 20))?
    try r.Close() catch err {
        println("close:", err.Message())
    }
    var zr = compress.zstd.NewReader()
    zr.Feed(cut(zs, 10))?
    println(zr.Done(), len(zr.Read()))
}
