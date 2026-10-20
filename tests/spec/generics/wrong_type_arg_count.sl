// error: type argument
// status: pending
namespace spec.generics.wrong_type_arg_count

import std.io

func Id<T>(x T) T {
    return x
}

func main() {
    println(Id<int, string>(1))
}
