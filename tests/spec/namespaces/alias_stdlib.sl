// test: aliases for stdlib namespaces, including nested ones
// status: passing
namespace spec.namespaces.alias_stdlib

import std.crypto.hash as h
import std.io
import std.strings as str

func main() {
    println(str.ToUpper("abc"), str.Repeat("-", 3))
    println(h.Md5Hex(""))
}
