// test: an instantiated generic function is a first-class value
// status: passing
namespace spec.generics.generic_func_value

import std.io

func Double<T: Ordered>(x T) T {
    return x + x
}

func main() {
    var di = Double<int>
    var ds = Double<string>
    var fs []func(int) int = [di, Double<int>]
    println(di(21), ds("ab"), fs[1](4))
}
