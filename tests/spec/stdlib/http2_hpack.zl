// test: std.net.http.hpack decodes and re-encodes the RFC 7541 appendix C examples (integers, literals, dynamic table eviction, Huffman)
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.http2_hpack

import std
import std.io
import std.net.http

func unhex(s string) []byte {
    return try encoding.hex.Decode(strings.Replace(s, " ", "")) else []
}

func show(fs []hpack.HeaderField) {
    for f in fs {
        println("  " + f.Name + ": " + f.Value)
    }
}

func tohex(b []byte) string {
    return encoding.hex.Encode(b)
}

func main() void! {
    // C.3: requests without Huffman, C.4: the same with Huffman
    var plain = [
        "8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d",
        "8286 84be 5808 6e6f 2d63 6163 6865",
        "8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65",
    ]
    var huff = [
        "8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff",
        "8286 84be 5886 a8eb 1064 9cbf",
        "8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf",
    ]
    for name, blocks in [plain, huff] {
        println("C." + fmt.Format("{0}", name + 3))
        var d = hpack.NewDecoder(4096)
        var e = hpack.NewEncoder(4096)
        e.Huffman = name == 1
        for i, b in blocks {
            var fs = d.Decode(unhex(b))?
            println(" request", i + 1, "table size", d.TableSize())
            show(fs)
            // our encoder makes the same choices for these examples
            println("  re-encoded identically:", tohex(e.Encode(fs)) == strings.Replace(b, " ", ""))
        }
    }
    // C.5: responses with a 256-byte table (evictions)
    var d = hpack.NewDecoder(256)
    var resp = [
        "4803 3330 3258 0770 7269 7661 7465 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 3a32 3120 474d 546e 1768 7474 7073 3a2f 2f77 7777 2e65 7861 6d70 6c65 2e63 6f6d",
        "4803 3330 37c1 c0bf",
        "88c1 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 3a32 3220 474d 54c0 5a04 677a 6970 7738 666f 6f3d 4153 444a 4b48 514b 425a 584f 5157 454f 5049 5541 5851 5745 4f49 553b 206d 6178 2d61 6765 3d33 3630 303b 2076 6572 7369 6f6e 3d31",
    ]
    println("C.5")
    for i, b in resp {
        var fs = d.Decode(unhex(b))?
        println(" response", i + 1, "table size", d.TableSize(), "entries", d.TableLen())
        show(fs)
    }
    // errors
    try d.Decode(unhex("80")) catch err {
        println("error:", err)
    }
    try d.Decode(unhex("ff ff ff ff ff ff")) catch err {
        println("error:", err)
    }
    try d.Decode(unhex("c5")) catch err {
        println("error:", err)
    }
    // sensitive fields are never indexed
    var e = hpack.NewEncoder(4096)
    var blk = e.Encode([hpack.HeaderField{Name: "authorization", Value: "secret", Sensitive: true}])
    var d2 = hpack.NewDecoder(4096)
    var back = d2.Decode(blk)?
    println("sensitive:", back[0].Sensitive, back[0].Value, "table entries", e.TableLen())
}
