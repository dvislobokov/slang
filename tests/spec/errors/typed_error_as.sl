// test: a struct with Message() is an error; errors.As extracts it through wrapping
// status: passing
namespace spec.errors.typed_error_as

import std.io
import std

struct HttpError {
    Code int
    func Message() string {
        return fmt.Format("http {0}", this.Code)
    }
}

func Fetch(path string) string! {
    if path == "/missing" {
        return HttpError{Code: 404}
    }
    return "body"
}

func Page(path string) string! {
    return Fetch(path)?("page " + path)
}

func main() {
    try Page("/missing") catch err {
        println(err)
        var he HttpError
        if errors.As(err, &he) {
            println("code", he.Code)
        }
        switch err {
            case HttpError e:
                println("direct", e.Code)
            default:
                println("wrapped, not a direct HttpError")
        }
    }
    var ok = try Page("/") else "?"
    println(ok)
}
