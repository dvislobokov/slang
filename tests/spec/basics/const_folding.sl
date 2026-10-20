// test: constants are folded at compile time, may depend on each other and size arrays
// status: passing
namespace spec.basics.const_folding

import std.io

const K = 1 << 10
const Twice = K * 2 + 1
const Greeting = "hello, " + "world"
const Ratio = 1.0 / 4

func main() {
    const Local = Twice - K
    var arr [3]int
    arr[2] = Local
    println(K, Twice, Local, Ratio)
    println(Greeting, len(Greeting), len(arr), arr)
    var f float = K
    println(f / 3)
}
