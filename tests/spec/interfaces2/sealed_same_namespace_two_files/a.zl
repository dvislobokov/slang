// test: implementations of a sealed interface may live in another file of the same namespace
// status: passing
namespace spec.interfaces2.sealed_same_namespace_two_files

import std.io

sealed interface Animal {
    func Sound() string
}

struct Dog {
    func Sound() string { return "woof" }
}

func Speak(a Animal) string {
    switch a {
        case Dog d: return "dog " + d.Sound()
        case Cow c: return "cow " + c.Sound()
    }
}

func main() {
    println(Speak(Dog{}))
    println(Speak(Cow{}))
}
