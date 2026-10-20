// test: fmt.Sprintf supports Go verbs %d %s %v %f with precision %x %q and padding
// status: passing
namespace spec.stdlib.fmt_sprintf

import std
import std.io

func main() {
    println(fmt.Sprintf("%d|%5d|%-4d|%03d", 42, 7, 3, 9))
    println(fmt.Sprintf("%s=%v %.2f %x %q", "k", true, 3.14159, 255, "hi"))
    println(fmt.Sprintf("%v %v %%", [1, 2], 1.5))
}
