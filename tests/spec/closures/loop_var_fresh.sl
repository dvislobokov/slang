// test: closures created in a loop capture a fresh variable per iteration (Go 1.22)
// status: passing
namespace spec.closures.loop_var_fresh

import std.io
import std

func main() {
    var fs []func() int = []
    for i in 0..3 {
        fs = append(fs, () => i * 10)
    }
    for _, x in ["a", "b"] {
        fs = append(fs, () => len(x))
    }
    var out = ""
    for f in fs {
        out += fmt.Format("{0};", f())
    }
    println(out)
}
