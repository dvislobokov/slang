// test: HMAC-SHA256 matches RFC 4231 test case 2; Equal compares in constant time
// status: passing
namespace spec.stdlib.crypto_hmac

import std.crypto.hmac
import std.io

func main() {
    println(Sha256Hex("Jefe", "what do ya want for nothing?"))
    var a = Sha256("k", "data")
    var b = Sha256("k", "data")
    var c = Sha256("k", "other")
    println(Equal(a, b), Equal(a, c), len(a))
}
