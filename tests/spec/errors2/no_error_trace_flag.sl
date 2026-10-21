// test: with --no-error-trace no frames are recorded: errors.Trace is "" and Frames is empty, messages are unchanged
// status: passing
// run-flags: --no-error-trace
namespace spec.errors2.no_error_trace_flag

import std
import std.io

func Fail() void! {
    return error("x")
}

func Up() void! {
    Fail()?("ctx")
}

func main() {
    try Up() catch err {
        println(err)
        println("[" + errors.Trace(err) + "]", len(errors.Frames(err)))
    }
}
