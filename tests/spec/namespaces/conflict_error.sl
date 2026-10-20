// error: ambiguous name Sha256Hex
// status: passing
namespace spec.namespaces.conflict_error

import std.crypto.hash
import std.crypto.hmac
import std.io

func main() {
    println(Sha256Hex("a"))
}
