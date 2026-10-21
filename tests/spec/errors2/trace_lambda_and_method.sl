// test: frames name methods as Type.Method and lambdas as parent$lambdaN
// status: passing
namespace spec.errors2.trace_lambda_and_method

import std
import std.io

struct Cache {
    func Get(k string) string! {
        return error("miss " + k)
    }
}

func main() {
    var c = Cache{}
    var f = func() string! {
        return c.Get("k")?
    }
    try f() catch err {
        for fr in errors.Frames(err) {
            println(fr.Func, fr.Line)
        }
    }
}
