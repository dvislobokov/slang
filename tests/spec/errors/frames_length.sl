// test: errors.Frames returns one record per trace line with File, Line, Func and Context
// status: pending
namespace spec.errors.frames_length

import std.io
import std

func Inner() void! {
    return error("x")
}

func Middle() void! {
    Inner()?("middle")
}

func Outer() void! {
    Middle()?
}

func main() {
    try Outer() catch err {
        var fs = errors.Frames(err)
        println(len(fs))
        for f in fs {
            println(f.Func, f.Line, "[" + f.Context + "]", f.File)
        }
    }
}
