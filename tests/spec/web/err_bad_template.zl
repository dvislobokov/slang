// error: invalid route "/files/{name": malformed parameter segment
// status: passing
namespace spec.web.err_bad_template

import std.web

func main() {
    var app = NewApp()
    app.MapGet("/files/{name", (name string) => name)
}
