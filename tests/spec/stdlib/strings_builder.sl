// test: strings.Builder accumulates text with Write and returns it with String
// status: pending
namespace spec.stdlib.strings_builder

import std.io
import std

func main() {
    var b = strings.Builder{}
    for i in 0..3 {
        b.Write(fmt.Format("[{0}]", i))
    }
    b.Write("!")
    println(b.String(), b.Len())
}
