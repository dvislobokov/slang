// test: os.Args() holds the program name followed by the arguments
// status: passing
// args: alpha beta "gamma"
namespace spec.stdlib.os_args

import std.io
import std

func main() {
    var args = os.Args()
    println(len(args))
    println(args[1..len(args)])
}
