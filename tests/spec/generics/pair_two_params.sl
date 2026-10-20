// test: a struct with two type parameters and a generic constructor function
// status: passing
namespace spec.generics.pair_two_params

import std.io

struct Pair<A, B> {
    First A
    Second B

    func Swap() Pair<B, A> {
        return Pair<B, A>{First: this.Second, Second: this.First}
    }
}

func MakePair<A, B>(a A, b B) Pair<A, B> {
    return Pair<A, B>{First: a, Second: b}
}

func main() {
    var p = MakePair("age", 42)
    var q = p.Swap()
    println(p.First, p.Second, q.First, q.Second)
}
