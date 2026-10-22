// error: attribute @get cannot be applied to a struct
// status: passing
namespace spec.web.err_attr_target

import std.web

@get("/x")
struct Bad {
    n int
}

func main() {
    var app = NewApp()
    _ = app
}
