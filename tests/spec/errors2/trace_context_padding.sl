// test: errors.Trace pads the function column to the longest name when a ?("context") follows
// status: passing
namespace spec.errors2.trace_context_padding

import std.io
import std

func ReadConfiguration() void! {
    return error("no file")
}

func Up() void! {
    ReadConfiguration()?("config")
}

func main() {
    try Up() catch err {
        println(errors.Trace(err))
    }
}
