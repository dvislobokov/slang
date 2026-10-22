// error: route parameter {when} has type Range
// status: passing
namespace spec.web.err_path_param_type

import std.web

struct Range {
    From int
    To   int
}

@route("/events")
struct EventsController {
    @get("{when}")
    func List(when Range) string {
        return "events"
    }
}

func main() {
    var app = NewApp()
    app.MapController<EventsController>()
}
