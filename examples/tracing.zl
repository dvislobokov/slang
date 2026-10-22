namespace examples.tracing

import std
import std.io
import std.net

// std.diag tracing: OpenTelemetry-style spans that follow the task
// context (children of the current span, also across tasks and HTTP calls
// through the W3C traceparent header), exported when they end. Seeded ids
// and a fake clock make the output reproducible.

var now = 1700000000000000000

func clock() int {
    now += 250000
    return now
}

async func Lookup(req *http.Request, res *http.Response) void! {
    // The client's span (from traceparent) is the parent of this one.
    var span = diag.StartSpanKind("GET /users", diag.SpanKind.Server)
    defer span.End()
    span.SetAttribute("http.route", "/users/{id}")
    res.Text(200, "alice")
}

async func load(id int) string! {
    var span = diag.StartSpan("load user")
    defer span.End()
    span.SetAttribute("user.id", id)
    var logger = log.New().Enrich.FromContext().WriteTo.Console(log.ConsoleOptions{
        Template: "log: {Message} trace={TraceId:l} span={SpanId:l}{NewLine}",
        Stdout: true,
    }).CreateLogger()
    logger.Info("loading {Id}", id)
    var res = await http.Get(base() + "/users/" + fmt.Format("{0}", id))?
    return res.Body
}

var port = 0

func base() string {
    return fmt.Format("http://127.0.0.1:{0}", port)
}

async func main() void! {
    diag.SetTraceSeed(2024, clock)
    diag.AddExporter(diag.ConsoleExporter{})

    var r = http.NewRouter()
    r.Get("/users/{id}", Lookup)
    var srv = http.Server{Addr: "127.0.0.1:0", Handler: r}
    srv.Listen()?
    var serving = srv.Serve()
    port = srv.Port()

    var root = diag.StartSpan("handle request")
    root.SetAttribute("client", "cli")
    var name = await load(7)?
    try fail() catch err {
        var s = diag.StartSpan("validate")
        s.RecordError(err)
        s.End()
    }
    root.SetStatus(diag.StatusCode.Ok, "")
    root.End()
    println("user:", name, "current span after End:", diag.CurrentSpan() == nil)
    println("traceparent:", root.Traceparent())
    println("otlp:", diag.OtlpSpanJson(root))

    await diag.Shutdown()
    await srv.Shutdown()
    await serving?
}

func fail() void! {
    return error("name too short")
}
