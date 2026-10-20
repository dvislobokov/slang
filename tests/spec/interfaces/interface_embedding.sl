// test: an interface may embed other interfaces, gaining their methods
// status: passing
namespace spec.interfaces.interface_embedding

import std.io

interface Reader {
    func Read() string
}

interface Writer {
    func Write(s string)
}

interface ReadWriter {
    Reader
    Writer
}

struct Buffer {
    Data string
    func Read() string { return this.Data }
    func Write(s string) { this.Data += s }
}

func Use(rw ReadWriter) {
    rw.Write("hello")
    rw.Write(" world")
    println(rw.Read())
}

func main() {
    Use(&Buffer{})
}
