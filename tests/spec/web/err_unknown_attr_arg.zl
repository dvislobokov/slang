// error: unknown argument maxLen for attribute @validate
// status: passing
namespace spec.web.err_unknown_attr_arg

import std.web

struct Signup {
    @validate(required: true, maxLen: 20) Name string
}

func main() {
    var app = NewApp()
    app.MapPost("/signup", (s Signup) => s.Name)
}
