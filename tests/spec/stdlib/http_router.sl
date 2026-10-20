// test: http.Server with http.NewRouter: path parameters, query values, 404/405, handler errors as 500, redirects followed by the client
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.http_router

import std.io
import std
import std.net

async func User(req *http.Request, res *http.Response) void! {
    res.Json(200, "{\"id\": " + req.Param("id") + ", \"tab\": \"" + req.QueryValue("tab") + "\"}")
}

async func Files(req *http.Request, res *http.Response) void! {
    res.Text(200, "file " + req.Param("*"))
}

async func Broken(req *http.Request, res *http.Response) void! {
    return error("boom")
}

async func Moved(req *http.Request, res *http.Response) void! {
    res.Status = 301
    res.SetHeader("location", "/users/9?tab=moved")
}

async func main() void! {
    var r = http.NewRouter()
    r.Get("/users/{id}", User)
    r.Get("/files/*", Files)
    r.Get("/broken", Broken)
    r.Get("/moved", Moved)
    var srv = http.Server{Addr: "127.0.0.1:0", Handler: r}
    srv.Listen()?
    var serving = srv.Serve()
    var base = fmt.Format("http://127.0.0.1:{0}", srv.Port())
    var paths = ["/users/42?tab=a%20b", "/files/a/b/c.txt", "/nope", "/broken", "/moved"]
    for p in paths {
        var res = await http.Get(base + p)?
        println(res.Status, res.Header("content-type"), strings.Trim(res.Body))
    }
    var post = await http.Post(base + "/users/1", "x", "text/plain")?
    println(post.Status, post.Header("Allow"))
    await srv.Shutdown()
    await serving?
}
