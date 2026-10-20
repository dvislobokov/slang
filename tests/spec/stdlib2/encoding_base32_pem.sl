// test: std.encoding.base32 (RFC 4648 vectors, hex alphabet, no padding) and std.encoding.pem blocks with headers
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.encoding_base32_pem

import std
import std.io
import std.encoding

func main() {
    for s in ["", "f", "fo", "foo", "foob", "fooba", "foobar"] {
        var e = base32.EncodeString(s)
        println("[" + e + "]", try base32.DecodeString(e) else "?", base32.EncodeHex(strings.Bytes(s)))
    }
    var np = base32.Std().WithPadding(false)
    println(np.EncodeString("fooba"), np.EncodeString("foob"), try np.DecodeString("MZXW6YQ") else "?", try base32.DecodeString("MZXW6YQ") else "pad required")
    println(try base32.DecodeString("MZXW6YQ=") else "bad length", try base32.DecodeString("M!======") else "bad char", try base32.DecodeString("MZXW\n6===") else "?")
    var hex = try base32.DecodeHex("CPNMUOJ1") else []
    println(strings.FromBytes(hex), base32.NewEncoding("abcdefghijklmnopqrstuvwxyz234567").EncodeString("hi"))

    var text = "junk before\n-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-128-CBC,ABCD\n\naGVsbG8g\nd29ybGQ=\n-----END RSA PRIVATE KEY-----\n-----BEGIN CERTIFICATE-----\nAQID\n-----END CERTIFICATE-----\ntrailing"
    var d = pem.Decode(text)
    if d.Block != nil {
        var b = d.Block
        println(b.Type, b.Header("Proc-Type"), b.Header("DEK-Info"), b.Header("None") == "", strings.FromBytes(b.Bytes), b.HeaderKeys)
    }
    println(strings.HasPrefix(d.Rest, "-----BEGIN CERTIFICATE"))
    var all = pem.DecodeAll(text)
    println(len(all), all[1].Type, all[1].Bytes)
    print(pem.Encode(all[0]))
    var blk = pem.Block{Type: "TEST"}
    for i in 0..50 {
        blk.Bytes = append(blk.Bytes, byte(i))
    }
    var enc = pem.Encode(blk)
    print(enc)
    var again = pem.Decode(enc)
    println(again.Block != nil && slices.Equal(again.Block.Bytes, blk.Bytes), again.Rest == "")
    var none = pem.Decode("no pem here")
    println(none.Block == nil, none.Rest)
    println(len(pem.DecodeAll("-----BEGIN X-----\n!!!\n-----END X-----\n")))
}
