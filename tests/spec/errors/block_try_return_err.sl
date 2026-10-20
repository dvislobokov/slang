// test: block try in a T! function; catch may re-propagate with return err
// status: pending
namespace spec.errors.block_try_return_err

import std.io
import std

func Sum(a, b string) int! {
    var r = 0
    try {
        r = strings.ToInt(a)? + strings.ToInt(b)?
    } catch err {
        if a == "" {
            return 0
        }
        return errors.Wrap(err, "sum")
    }
    return r
}

func main() {
    println(try Sum("2", "3") else -1)
    println(try Sum("", "3") else -1)
    try Sum("x", "3") catch err {
        println(strings.HasPrefix(err.Message(), "sum: "))
    }
}
