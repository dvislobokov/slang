// test: ?("ctx") wraps: message is "ctx: inner", the cause stays reachable
// status: passing
namespace spec.errors.question_with_context

import std
import std.io

const ErrMissing = error("missing")

func Read(key string) string! {
    if key == "" {
        return ErrMissing
    }
    return "v:" + key
}

func Load(key string) string! {
    return Read(key)?("loading " + key)
}

func Boot() string! {
    var v = Load("")?("boot")
    return v
}

func main() {
    try Boot() catch err {
        println(err)
        println(errors.Is(err, ErrMissing))
        println(errors.Unwrap(err))
        println(errors.Unwrap(errors.Unwrap(err)))
    }
}
