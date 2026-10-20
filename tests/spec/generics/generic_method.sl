// test: a generic method on a non-generic struct, inferred and explicit
// status: passing
namespace spec.generics.generic_method

import std.io
import std

struct Logger {
    Prefix string

    func Show<T>(x T) string {
        return fmt.Format("{0}{1}", this.Prefix, x)
    }
}

func main() {
    var l = Logger{Prefix: "> "}
    println(l.Show(5))
    println(l.Show<string>("text"))
    println(l.Show(true))
}
