// error: wrong number of type arguments for Id: expected 1, got 2
// status: passing
namespace spec.generics2.wrong_type_arg_count_wording

import std.io

func Id<T>(x T) T { return x }

func main() {
    println(Id<int, string>(1))
}
