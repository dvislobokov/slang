// test: web.Result<T> results (Ok, Created, CreatedAt, Accepted, NoContent, StatusCode, WithHeader, NotFound inferred from the declared result), plain values, void and T! handlers, async handlers, content negotiation (JSON by default, XML and YAML by Accept, text/plain for strings) and the typed test client
// status: passing
// run-flags: --leak-check
namespace spec.web.results

import std
import std.io
import std.web

struct Point {
    X int
    Y int
}

@route("/points")
struct PointsController {
    @get("{id}")
    func Get(id int) Result<Point> {
        if id == 0 {
            return NotFound()
        }
        return Ok(Point{X: id, Y: id * 2}).WithHeader("X-Version", "3")
    }

    @post("")
    func Create(@body p Point) Result<Point> {
        return CreatedAt("/points/" + fmt.Format("{0}", p.X), p)
    }

    @put("{id}")
    func Replace(id int) Result<Point> {
        return Accepted(Point{X: id})
    }

    @delete("{id}")
    func Delete(id int) Result<Point> {
        if id < 0 {
            return StatusCode(410)
        }
        return NoContent()
    }
}

async func double(n int) int! {
    await task.Delay(1)
    if n < 0 {
        return error("negative")
    }
    return n * 2
}

async func main() void! {
    var app = NewAppWith(AppOptions{Logger: log.New().MinimumLevel(log.Level.Fatal).CreateLogger()})
    app.MapController<PointsController>()
    app.MapGet("/value", () => Point{X: 1, Y: 2})
    app.MapGet("/text", () => "plain")
    app.MapGet("/void", func(c *Context) {
        c.SetHeader("X-Done", "yes")
    })
    app.MapGet("/nothing", func() {})
    app.MapGet("/double/{n}", async func(n int) int! {
        return await double(n)?
    })
    app.MapGet("/list", () => [Point{X: 1}, Point{X: 2}])
    var c = TestServer(app)?
    var r = await c.Get("/points/4")
    println(r.Status, r.Header("Content-Type"), r.Header("X-Version"), r.Body)
    r = await c.Get("/points/0")
    println(r.Status, r.Body)
    r = await c.Post("/points", "{\"X\": 7, \"Y\": 8}")
    println(r.Status, r.Header("Location"), r.Body)
    r = await c.Put("/points/5", "")
    println(r.Status, r.Body)
    r = await c.Delete("/points/5")
    println(r.Status, "[" + r.Body + "]")
    r = await c.Delete("/points/-1")
    println(r.Status, "[" + r.Body + "]")
    r = await c.WithHeader("Accept", "application/xml").Get("/points/4")
    println(r.Status, r.Header("Content-Type"), r.Body)
    r = await c.WithHeader("Accept", "application/yaml").Get("/points/4")
    println(r.Status, r.Header("Content-Type"))
    print(r.Body)
    for path in ["/value", "/text", "/void", "/nothing", "/double/21", "/double/-1", "/list"] {
        r = await c.Get(path)
        println(path, "->", r.Status, r.Header("Content-Type"), r.Header("X-Done"), r.Body)
    }
    var p = await c.GetJSON<Point>("/points/9")?
    println("typed:", p.X, p.Y)
    try await c.GetJSON<Point>("/points/0") catch err {
        println("typed error:", err)
    }
    var made = await c.PostJSON<Point, Point>("/points", Point{X: 3, Y: 4})?
    println("posted:", made.X, made.Y)
}
