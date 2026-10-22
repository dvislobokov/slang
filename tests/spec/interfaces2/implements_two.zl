// test: `implements A, B` checks several interfaces
// status: passing
namespace spec.interfaces2.implements_two

import std.io

interface Reader {
    func Read() string
}

interface Writer {
    func Write(s string)
}

struct Pipe implements Reader, Writer {
    buf string
    func Read() string { return this.buf }
    func Write(s string) { this.buf += s }
}

func main() {
    var p = &Pipe{}
    var w Writer = p
    var r Reader = p
    w.Write("ping")
    w.Write("pong")
    println(r.Read())
}
