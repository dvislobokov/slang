// test: std.web middleware: Use order (outermost first) and short-circuiting, request ids (log property RequestId), request logging through the app logger, std.diag server spans, body limits, recover of middleware errors, CORS preflights, gzip, static files, rate limiting
// status: passing
// run-flags: --leak-check
namespace spec.web.middleware

import std
import std.io
import std.net
import std.web

var now = 1700000000000000000

func clock() int {
    now += 1000
    return now
}

var ids = 0

func nextId() string {
    ids += 1
    return fmt.Format("id-{0}", ids)
}

async func main() void! {
    diag.SetTraceSeed(7, clock)
    diag.AddExporter(diag.ConsoleExporter{})
    var logger = log.New().Enrich.FromContext().WriteTo.Console(log.ConsoleOptions{
        Template: "log: {RequestMethod:l} {RequestPath:l} {StatusCode} route={Route:l} id={RequestId:l}{NewLine}",
        Stdout: true,
    }).CreateLogger()
    var app = NewAppWith(AppOptions{Logger: logger})
    app.Use(async func(c *Context, next *Next) void! {
        println("first: before")
        await next.Run(c)?
        println("first: after", c.Response.Status)
    })
    app.UseRequestId(RequestIdOptions{Generate: nextId})
    app.UseLogging()
    app.UseTracing()
    app.UseRecover()
    app.Use(async func(c *Context, next *Next) void! {
        if c.Request.Header("X-Block") != "" {
            println("second: blocked")
            c.Text(403, "blocked")
            return
        }
        if c.Request.Header("X-Fail") != "" {
            return error("middleware failed")
        }
        println("second: before")
        await next.Run(c)?
        println("second: after")
    })
    app.UseBodyLimit(10)
    app.UseCors(http.CORSOptions{AllowedOrigins: ["https://app.example"]})
    app.UseGzip()
    app.UseStaticFiles("/static", "/tmp/slang-web-spec-static")
    app.MapGet("/hello/{name}", (name string) => "hello " + name)
    app.MapGet("/big", () => strings.Repeat("abc", 1000))
    app.MapPost("/echo", func(c *Context) string {
        return c.Request.Body
    })
    fs.MkdirAll("/tmp/slang-web-spec-static")?
    io.WriteFile("/tmp/slang-web-spec-static/a.txt", "static file")?
    var cli = TestServer(app)?

    var r = await cli.Get("/hello/ann")
    println("->", r.Status, r.Body, r.Header("X-Request-Id"))
    r = await cli.WithHeader("X-Request-Id", "given").WithHeader("X-Block", "1").Get("/hello/ann")
    println("->", r.Status, r.Body, r.Header("X-Request-Id"))
    r = await cli.WithHeader("X-Fail", "1").Get("/hello/ann")
    println("->", r.Status, r.Body)
    r = await cli.Post("/echo", "0123456789abc")
    println("->", r.Status, r.Body)
    r = await cli.Post("/echo", "short")
    println("->", r.Status, r.Body)
    r = await cli.WithHeader("Origin", "https://app.example").WithHeader(
        "Access-Control-Request-Method",
        "POST",
    ).Send("OPTIONS", "/echo", "", nil)
    println(
        "preflight ->",
        r.Status,
        r.Header("Access-Control-Allow-Origin"),
        r.Header("Access-Control-Allow-Methods"),
    )
    r = await cli.WithHeader("Accept-Encoding", "gzip").Get("/big")
    println("gzip ->", r.Status, r.Header("Content-Encoding"), len(r.Body) < 3000)
    r = await cli.Get("/static/a.txt")
    println("static ->", r.Status, r.Body)
    r = await cli.Get("/static/missing.txt")
    println("static missing ->", r.Status)

    var limited = NewApp()
    limited.UseRateLimit(RateLimitOptions{Rate: 0.5, Burst: 2, Clock: () => 5000})
    limited.MapGet("/", () => "ok")
    var lc = TestServer(limited)?
    for i in 0..3 {
        var x = await lc.Get("/")
        println("rate", i, "->", x.Status, x.Header("Retry-After"))
    }
}
