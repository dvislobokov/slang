// error: duplicate route POST /orders
// status: passing
namespace spec.web.err_duplicate_minimal

import std.web

func main() {
    var app = NewApp()
    app.MapPost("/orders", () => "a")
    app.MapPost("/orders/", () => "b")
}
