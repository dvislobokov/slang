// test: std.compress decodes streams made by other tools: gzip(1) files (with FNAME/MTIME, -1 -n, two concatenated members), Python zlib (zlib and raw deflate), libzstd 1.5 (level 19 with checksum, a skippable frame, two frames); checksum known answers (CRC-32, Adler-32)
// status: passing
namespace spec.stdlib3.compress_vectors

import std
import std.io

func text(b []byte) string {
    return strings.FromBytes(b)
}

func main() void! {
    var named = encoding.base64.Decode("H4sICCV9k2UCA2ZveC50eHQAC8lIVSgszUzOVkgqyi/PU0jLr1DIKs0tKFbIL0stUigBSuckVlUqpOSn63GFkKIYAMd4CDRaAAAA")?
    var h = compress.gzip.ReadHeader(named)?
    println(h.Name, h.ModTime, h.OS, h.Comment == "")
    print(text(compress.gzip.Decompress(named)?))
    var n1 = encoding.base64.Decode("H4sIAAAAAAAEAwvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9LLVIoAUrnJFZVKqTkp+txhZCiGADHeAg0WgAAAA==")?
    println(len(compress.gzip.Decompress(n1)?))
    var multi = encoding.base64.Decode("H4sIAAAAAAAEAwvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9LLVIoAUrnJFZVKqTkp+txhZCiGADHeAg0WgAAAB+LCAAAAAAAAAMrTk3Oz0tRyE3NTUot4gIANhhLDg4AAAA=")?
    var all = text(compress.gzip.Decompress(multi)?)
    println(len(all), strings.HasSuffix(all, "second member\n"))

    var z = encoding.base64.Decode("eNoLyUhVKCzNTM5WSCrKL89TSMuvUMgqzS0oVsgvSy1SKAFK5yRWVSqk5KfrcYWQohgAyxYgIw==")?
    println(len(compress.zlib.Decompress(z)?))
    var raw = encoding.base64.Decode("C8lIVSgszUzOVkgqyi/PU0jLr1DIKs0tKFbIL0stUigBSuckVlUqpOSn63GFkKIYAA==")?
    println(text(compress.deflate.Decompress(raw)?) == text(compress.zlib.Decompress(z)?))

    var zs = encoding.hex.Decode("28b52ffd245aad0100d40254686520717569636b2062726f776e20666f78206a756d7073206f76657220746865206c617a7920646f672e0a0100159aaa0ca7a5f2a4")?
    println(compress.zstd.IsFrame(zs), text(compress.zstd.Decompress(zs)?) == text(compress.zlib.Decompress(z)?))
    var two = encoding.hex.Decode("28b52ffd245aad0100d40254686520717569636b2062726f776e20666f78206a756d7073206f76657220746865206c617a7920646f672e0a0100159aaa0ca7a5f2a4" + "502a4d1803000000abcdef" + "28b52ffd200a5100006672616d652074776f0a")?
    var out = text(compress.zstd.Decompress(two)?)
    println(len(out), strings.HasSuffix(out, "frame two\n"))

    println(compress.Crc32(0, strings.Bytes("123456789")), compress.Adler32(1, strings.Bytes("Wikipedia")))
    var part = compress.Crc32(0, strings.Bytes("12345"))
    println(compress.Crc32(part, strings.Bytes("6789")) == compress.Crc32(0, strings.Bytes("123456789")))
}
