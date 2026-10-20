// test: a @reflect type keeps full metadata, visible through typeof on an any
// status: pending
namespace spec.reflection.reflect_retention

import std.io
import std

@reflect
struct Plugin {
    Name string
    Version int

    func Run() string {
        return "running " + this.Name
    }
}

func Inspect(x any) {
    var t = typeof(x)
    println(t.FullName, len(t.Fields), len(t.Methods))
    for m in t.Methods {
        println("method", m.Name)
    }
}

func main() {
    Inspect(Plugin{Name: "p", Version: 1})
}
