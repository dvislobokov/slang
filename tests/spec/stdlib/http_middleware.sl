// test: Router.Use middleware chains (order), http.Recover, http.Logging through a std.log sink, CORS (preflight and simple requests), http.Gzip, http.BodyLimit, http.TimeoutHandler, Server.MaxBodyBytes; handlers called directly with std.net.http.test recorders
// status: passing
// run-flags: --leak-check
// stderr: EROR HTTP GET /fail failed: secret detail
namespace spec.stdlib.http_middleware

import std.io
import std
import std.net

// A std.log sink that prints the request-log properties (not the timing).
struct PrintSink {
    func Emit(e *log.Event) {
        println("log:", e.Level, e.Get("RequestMethod"), e.Get("RequestPath"), e.Get("StatusCode"), e.Error != nil)
    }

    func Close() Task<void> {
        return task.Delay(0)
    }
}

// A middleware that records its position.
func tag(name string) func(http.Handler) http.Handler {
    return func(next http.Handler) http.Handler {
        return http.Func(func(req *http.Request, res *http.Response) Task<void!> {
            res.AddHeader("X-Chain", name)
            return next.Serve(req, res)
        })
    }
}

async func main() void! {
    var logger = log.New().WriteTo.Sink(PrintSink{}).CreateLogger()
    var r = http.NewRouter()
    r.Use(tag("outer"))
    r.Use(tag("inner"))
    r.Use(http.Logging(logger))
    r.Use(http.Recover())
    r.Use(http.CORS(http.CORSOptions{AllowedOrigins: ["https://app.example"], MaxAge: 600}))
    r.Use(http.Gzip(0))
    r.Use(http.BodyLimit(16))
    r.Get("/ok", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, "ok")
    })
    r.Get("/fail", async func(req *http.Request, res *http.Response) void! {
        return error("secret detail")
    })
    r.Get("/big", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, strings.Repeat("all work and no play ", 200))
    })
    r.Post("/upload", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, fmt.Format("{0} bytes", len(req.Body)))
    })

    // recorders: no network
    var rec = await http.test.Serve(r, http.test.NewRequest("GET", "/ok", ""))
    println("ok:", rec.Status, rec.Body, rec.Header("X-Chain"))
    rec = await http.test.Serve(r, http.test.NewRequest("GET", "/fail", ""))
    print("fail: ", rec.Status, " ", rec.Body)
    var pre = http.test.NewRequest("OPTIONS", "/ok", "")
    pre.Headers["Origin"] = "https://app.example"
    pre.Headers["Access-Control-Request-Method"] = "PUT"
    pre.Headers["Access-Control-Request-Headers"] = "X-Token"
    rec = await http.test.Serve(r, pre)
    println("preflight:", rec.Status, rec.Header("Access-Control-Allow-Origin"), "|", rec.Header("Access-Control-Allow-Methods"), "|", rec.Header("Access-Control-Allow-Headers"), "|", rec.Header("Access-Control-Max-Age"))
    var simple = http.test.NewRequest("GET", "/ok", "")
    simple.Headers["Origin"] = "https://app.example"
    rec = await http.test.Serve(r, simple)
    println("simple CORS:", rec.Header("Access-Control-Allow-Origin"), rec.Header("Vary"))
    var other = http.test.NewRequest("GET", "/ok", "")
    other.Headers["Origin"] = "https://evil.example"
    rec = await http.test.Serve(r, other)
    println("other origin: [" + rec.Header("Access-Control-Allow-Origin") + "]")
    var gz = http.test.NewRequest("GET", "/big", "")
    gz.Headers["Accept-Encoding"] = "gzip, br"
    rec = await http.test.Serve(r, gz)
    var plain = compress.gzip.Decompress(strings.Bytes(rec.Body))?
    println("gzip:", rec.Header("Content-Encoding"), len(rec.Body) < 200, len(plain))
    rec = await http.test.Serve(r, http.test.NewRequest("POST", "/upload", "0123456789abcdefXYZ"))
    print("body limit: ", rec.Status, " ", rec.Body)

    // TimeoutHandler
    var slow = http.Func(async func(req *http.Request, res *http.Response) void! {
        await task.Delay(500)
        res.Text(200, "too late")
    })
    var fast = http.Func(async func(req *http.Request, res *http.Response) void! {
        res.Text(201, "in time")
    })
    rec = await http.test.Serve(http.TimeoutHandler(slow, 30, "timed out\n"), http.test.NewRequest("GET", "/", ""))
    print("timeout: ", rec.Status, " ", rec.Body)
    rec = await http.test.Serve(http.TimeoutHandler(fast, 1000, "timed out\n"), http.test.NewRequest("GET", "/", ""))
    println("in time:", rec.Status, rec.Body)

    // over the network, with a server body limit
    var ts = http.test.NewServerWith(r, http.test.Options{MaxBodyBytes: 1000})?
    var res = await ts.Client.Get(ts.URL + "/ok")?
    println("network:", res.Status, res.Body, res.Header("X-Chain"))
    var huge = await ts.Client.Post(ts.URL + "/upload", strings.Repeat("x", 5000), "text/plain")?
    print("MaxBodyBytes: ", huge.Status, " ", huge.Body)
    await ts.Close()
}
