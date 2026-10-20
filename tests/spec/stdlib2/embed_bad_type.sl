// error: @embed works on string, []byte and embed.FS, not int
// status: passing
namespace spec.stdlib2.embed_bad_type

import std.io

@embed("embed_bad_type.sl")
var Data int

func main() {
    println(Data)
}
