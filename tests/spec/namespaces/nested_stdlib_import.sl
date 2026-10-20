// test: import std.crypto gives hash.X; import std.crypto.hash gives bare X
// status: passing
namespace spec.namespaces.nested_stdlib_import

import std.io
import std.crypto
import std.crypto.hash

func main() {
    println(hash.Sha1Hex("abc") == Sha1Hex("abc"), hash.Md5Hex("") == Md5Hex(""))
    println(std.crypto.hash.Crc32("123456789"))
}
