// test: crypto random bytes have the requested length and differ between calls
// status: passing
namespace spec.stdlib.crypto_rand

import std.crypto
import std.io

func main() {
    var a = try rand.Hex(16) else ""
    var b = try rand.Hex(16) else ""
    var bytes = try rand.Bytes(5) else []
    println(len(a), a != b, len(bytes))
}
