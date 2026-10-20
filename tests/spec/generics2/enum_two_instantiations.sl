// test: two instantiations of a generic enum (Option<int>, Option<string>) matched in one function
// status: passing
namespace spec.generics2.enum_two_instantiations

import std.io

enum Option<T> {
    Some(value T)
    None
}

func Lookup(id int) Option<string> {
    if id == 1 {
        return Option<string>.Some("alice")
    }
    return Option<string>.None
}

func Describe(o Option<int>) string {
    switch o {
        case Option.Some(id):
            switch Lookup(id) {
                case Option.Some(name): return "user " + name
                default: return "unknown user"
            }
        default: return "no id"
    }
}

func main() {
    println(Describe(Option<int>.Some(1)))
    println(Describe(Option<int>.Some(2)))
    println(Describe(Option<int>.None))
}
