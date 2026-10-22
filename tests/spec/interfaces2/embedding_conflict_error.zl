// error: conflicting signatures for method Close
// status: passing
namespace spec.interfaces2.embedding_conflict_error

import std.io

interface Closer {
    func Close()
}

interface Reader {
    func Close() bool
}

interface ReadCloser {
    Reader
    Closer
}

func main() {
    var x ReadCloser
    println(x == nil)
}
