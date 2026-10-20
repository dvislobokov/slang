// test: stdlib attribute structs are named through their own namespace's import (`@flag` with import std.cli, `@binary` with import std.encoding.binary); builtin `@json` still works with import std.reflect; their fields are readable through Attribute<A>()
// status: passing
namespace spec.attributes2.stdlib_attribute_names

import std.io
import std.cli
import std.reflect
import std.encoding.binary

@binary(endian: "big")
struct Opts {
    @flag(name: "port", short: "p") @json(name: "listen_port") Port int = 8080
    @binary(endian: "little") @json(skip: true) Magic uint32
}

func main() {
    var t = TypeOf<Opts>()
    var f = t.Fields[0].Attribute<flag>()
    println(f.name, f.short, f.required)
    println(t.Fields[0].Attribute<json>().name, t.Fields[1].Attribute<json>().skip)
    println(t.Attribute<binary>().endian, t.Fields[1].Attribute<binary>().endian, t.Fields[1].Attribute<flag>() == nil)
}
