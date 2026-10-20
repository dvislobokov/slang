// test: a trace longer than the inline frame array (15 frames) keeps every frame in order
// status: passing
namespace spec.errors2.trace_many_frames

import std.io
import std

func Dive(n int) int! {
    if n == 0 {
        return error("bottom")
    }
    var v = Dive(n - 1)?
    return v + 1
}

func main() {
    try Dive(13) catch err {
        var fs = errors.Frames(err)
        println(err, len(fs))
        println(fs[0].Func, fs[0].Line, fs[1].Line, fs[13].Func, fs[13].Line, fs[14].Func, fs[14].Line)
        var lines = strings.Split(errors.Trace(err), "\n")
        println(len(lines))
        println(lines[0])
        println(lines[14])
    }
}
