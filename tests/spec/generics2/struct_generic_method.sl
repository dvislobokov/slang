// test: a generic struct with a generic method: Box<T>.Map<U> returns Box<U>, inferred from the lambda and explicit
// status: passing
namespace spec.generics2.struct_generic_method

import std.io
import std

struct Box<T> {
    V T

    func Map<U>(f func(T) U) Box<U> {
        return Box<U>{V: f(this.V)}
    }

    func Get() T { return this.V }
}

func main() {
    var b = Box<int>{V: 3}
    var s = b.Map((x int) => strings.Repeat("ab", x))
    println(s.Get(), len(s.Get()))
    var f = b.Map<float>((x int) => float(x) / 2.0)
    println(f.V)
    var back = s.Map((t string) => len(t)).Map((n int) => n > 5)
    println(back.V)
}
