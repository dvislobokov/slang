// test: std.compress.zstd Compress/CompressNoChecksum: round trips through Decompress for empty, small, text, binary, run and multi-block inputs at several levels; frames start with the magic number; text compresses; the streaming Reader reads concatenated frames
// status: passing
// run-flags: --leak-check
namespace spec.stdlib4.zstd_encode

import std
import std.io

func same(a []byte, b []byte) bool {
    if len(a) != len(b) {
        return false
    }
    for i, x in a {
        if x != b[i] {
            return false
        }
    }
    return true
}

func main() void! {
    var text = strings.Repeat("The quick brown fox jumps over the lazy dog. ", 4000)
    var bin []byte = []
    var x = 12345
    for _ in 0..200000 {
        x = (x * 1103515245 + 12345) % 2147483648
        bin = append(bin, byte((x >> 16) % 17 + 100))
    }
    var runs = strings.Bytes(strings.Repeat("a", 300000))
    var inputs = [
        strings.Bytes(""),
        strings.Bytes("a"),
        strings.Bytes("hello, zstd"),
        strings.Bytes(text),
        bin,
        runs,
    ]
    for i, data in inputs {
        for level in [1, 3, 9, 19] {
            var c = compress.zstd.Compress(data, level)?
            var d = compress.zstd.Decompress(c)?
            if !same(d, data) || !compress.zstd.IsFrame(c) {
                println("MISMATCH", i, level)
            }
        }
        var c = compress.zstd.CompressNoChecksum(data, 0)?
        println(i, len(data), same(compress.zstd.Decompress(c)?, data), len(c) < len(data) + 16)
    }
    var small = compress.zstd.Compress(strings.Bytes(text), 3)?
    println("text ratio below 2%", len(small) * 50 < len(text))
    var r = compress.zstd.NewReader()
    r.Feed(
        compress.rtConcat(
            compress.zstd.Compress(strings.Bytes("first "), 1)?,
            compress.zstd.Compress(strings.Bytes("second"), 1)?,
        ),
    )?
    println(strings.FromBytes(r.Read()), r.Done())
}
