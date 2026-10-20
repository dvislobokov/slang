// test: func main() void! returning an error prints it to stderr and exits with code 1
// status: passing
// exit: 1
// stderr: config missing
namespace spec.async.main_error_exit

import std.io
import std

func Load() string! {
    return error("config missing")
}

func main() void! {
    println("start")
    var c = Load()?
    println(c)
}
