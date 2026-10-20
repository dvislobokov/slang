// error: help: add `?` to pass the error to the caller, or handle it here with `try ... catch err { ... }`
// status: passing
namespace spec.diagnostics.help_unhandled_result

func parse(s string) int! {
    if s == "" {
        return error("empty")
    }
    return 1
}

func load() int! {
    var n = parse("x")
    return n
}

func main() {
    _ = load()
}
