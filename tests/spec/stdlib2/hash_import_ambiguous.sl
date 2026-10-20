// error: ambiguous name hash: imported from both std and std.crypto
// status: passing
namespace spec.stdlib2.hash_import_ambiguous

import std
import std.crypto
import std.io

// `import std` gives std.hash as `hash`, `import std.crypto` gives
// std.crypto.hash as `hash`: alias one (`import std.crypto.hash as chash`)
// or write the full path (`crypto.hash.Sha256Hex`).
func main() {
    println(hash.Sha256Hex("abc"))
}
