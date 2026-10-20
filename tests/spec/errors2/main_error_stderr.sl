// test: an error returned from main is printed to stderr as `error: <message>`, exit code 1
// status: passing
// exit: 1
// stderr: error: save: disk full
namespace spec.errors2.main_error_stderr

import std.io

func Save() void! {
    return error("disk full")
}

func main() void! {
    println("saving")
    Save()?("save")
    println("not reached")
}
