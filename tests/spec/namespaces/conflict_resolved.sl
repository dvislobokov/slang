// test: a short-name conflict is resolved with an alias or the full path
// status: passing
namespace spec.namespaces.conflict_resolved

import std.io
import std.crypto.hash
import std.crypto.hmac as mac

func main() {
    println(len(Sha256Hex("a")), len(mac.Sha256Hex("k", "a")), len(std.crypto.hmac.Sha256Hex("k", "a")))
}
