// error: GET requests have none
// status: passing
namespace spec.web.err_get_body

import std.web

struct Filter {
    Tag string
}

func main() {
    var app = NewApp()
    app.MapGet("/search", (f Filter) => f.Tag)
}
