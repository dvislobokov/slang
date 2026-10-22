// error: HomeController has no @route("/prefix") attribute
// status: passing
namespace spec.web.err_no_route_attr

import std.web

struct HomeController {
    @get("")
    func Index() string {
        return "home"
    }
}

func main() {
    var app = NewApp()
    app.MapController<HomeController>()
}
