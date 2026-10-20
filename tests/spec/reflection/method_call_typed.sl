// test: the typed form m.Call<F>() returns a plain function value
// status: pending
namespace spec.reflection.method_call_typed

import std.io
import std

struct Counter {
    N int

    func Add(d int) int {
        this.N += d
        return this.N
    }
}

func main() {
    var c = Counter{N: 1}
    var add = reflect.TypeOf<Counter>().Method("Add").Call<func(*Counter, int) int>()
    println(add(&c, 5), add(&c, 10), c.N)
}
