// test: os.Exit(n) ends the program immediately with code n; pending output is flushed, defers do not run
// status: passing
// exit: 3
namespace spec.stdlib.os_exit_code

import std
import std.io

func main() {
    defer println("deferred: must not run")
    println("before exit")
    os.Exit(3)
    println("after exit")
}
