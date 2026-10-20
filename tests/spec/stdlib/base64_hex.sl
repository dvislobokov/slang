// test: base64 (standard and URL alphabets, with padding) and hex encoding/decoding (RFC 4648 vectors)
// status: passing
namespace spec.stdlib.base64_hex

import std.encoding
import std.io

func main() {
    println(
        base64.EncodeString(""),
        base64.EncodeString("f"),
        base64.EncodeString("fo"),
        base64.EncodeString("foobar"),
    )
    println(
        try base64.DecodeString("aGVsbG8=") else "?",
        try base64.DecodeString("!!") else "invalid",
    )
    var raw []byte = [251, 255, 191]
    println(base64.Encode(raw), base64.EncodeURL(raw))
    println(
        hex.EncodeString("hi!"),
        hex.Encode(raw),
        try hex.DecodeString("6869") else "?",
        try hex.DecodeString("zz") else "invalid",
    )
}
