// test: http.Client features: the request builder (Header/Query/Body string, []byte, io.Reader/JSON<T>/Form), Response.JSON<T>, transparent gzip, a CookieJar across redirects, streamed bodies (Client.Stream, BodyReader), total timeouts through the task context, http.GetJSON<T>
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.http_pool_client

import std.io
import std
import std.net

struct Item {
    Name string
    Count int
}

async func main() void! {
    var r = http.NewRouter()
    r.Post("/echo", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, req.Method + " " + req.RequestURI() + " ct=" + req.Header("Content-Type") + " x=" + req.Header("X-Test") + " body=" + req.Body)
    })
    r.Post("/item", async func(req *http.Request, res *http.Response) void! {
        var it = req.JSON<Item>()?
        it.Count += 1
        res.SendJSON<Item>(200, it)
    })
    r.Get("/text", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, strings.Repeat("compress me ", 500))
    })
    r.Get("/login", async func(req *http.Request, res *http.Response) void! {
        res.SetCookie(http.Cookie{Name: "session", Value: "abc123", Path: "/", HttpOnly: true})
        res.SetCookie(http.Cookie{Name: "theme", Value: "dark", Path: "/me"})
        res.Redirect(302, "/me")
    })
    r.Get("/me", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, "session=" + req.Cookie("session") + " theme=" + req.Cookie("theme"))
    })
    r.Get("/chunks", async func(req *http.Request, res *http.Response) void! {
        for i in 0..4 {
            res.Write(fmt.Format("[chunk {0}]", i))
            await res.Flush()?
        }
    })
    r.Get("/slow", async func(req *http.Request, res *http.Response) void! {
        await task.Delay(600)
        res.Text(200, "late")
    })
    r.Get("/item/7", async func(req *http.Request, res *http.Response) void! {
        res.SendJSON<Item>(200, Item{Name: "seven", Count: 7})
    })
    var mws []func(http.Handler) http.Handler = [http.Gzip(6)]
    var ts = http.test.NewServer(http.Chain(r, mws))?
    var c = ts.Client

    // the builder
    var b = http.NewRequest("POST", ts.URL + "/echo").Header("X-Test", "1").Query("q", "a b").Query("n", "2").Body("plain")
    var res = await c.Send(b)?
    println(res.Body)
    res = await c.Send(http.NewRequest("POST", ts.URL + "/echo").Body(strings.Bytes("bytes")).ContentType("application/octet-stream"))?
    println(res.Body)
    res = await c.Send(http.NewRequest("POST", ts.URL + "/echo").Body(io.NewStringReader("from a reader")))?
    println(res.Body)
    var form = map[string]string{}
    form["b"] = "2 3"
    form["a"] = "1&"
    res = await c.Send(http.NewRequest("POST", ts.URL + "/echo").Form(form))?
    println(res.Body)
    res = await c.Send(http.NewRequest("POST", ts.URL + "/item").JSON<Item>(Item{Name: "widget", Count: 1}))?
    var it = res.JSON<Item>()?
    println("json:", it.Name, it.Count, res.Header("Content-Type"))
    var seven = await http.GetJSON<Item>(ts.URL + "/item/7")?
    println("GetJSON:", seven.Name, seven.Count)

    // gzip: decoded transparently unless compression is disabled
    var g = await c.Get(ts.URL + "/text")?
    println("gzip:", len(g.Body), "encoding [" + g.Header("Content-Encoding") + "]", strings.HasPrefix(g.Body, "compress me"))
    var raw = http.Transport{DisableCompression: true}
    var rc = http.Client{Transport: &raw}
    var g2 = await rc.Send(http.NewRequest("GET", ts.URL + "/text").Header("Accept-Encoding", "gzip"))?
    println("raw gzip:", len(g2.Body) < 200, g2.Header("Content-Encoding"))

    // cookies across a redirect
    var jar = http.NewCookieJar()
    var jc = http.Client{Transport: ts.Transport, Jar: jar}
    var me = await jc.Get(ts.URL + "/login")?
    println("jar:", me.Status, me.Body, "stored", jar.Len())
    var u = http.ParseURL(ts.URL + "/other")?
    println("cookies for /other:", len(jar.Cookies(u)))

    // a streamed body
    var s = await c.Stream(http.NewRequest("GET", ts.URL + "/chunks"))?
    var pieces = 0
    var all = ""
    for {
        var chunk = await s.Reader.Read(5)?
        if len(chunk) == 0 {
            break
        }
        pieces += 1
        all += strings.FromBytes(chunk)
    }
    println("stream:", all, pieces > 4)

    // a total timeout cancels the request
    var t0 = time.Now()
    try await c.Send(http.NewRequest("GET", ts.URL + "/slow").Timeout(100)) catch err {
        println("timeout:", errors.Is(err, task.ErrCancelled), time.Now() - t0 < 500)
    }
    var tc = http.Client{Transport: ts.Transport, TotalTimeout: 100}
    try await tc.Get(ts.URL + "/slow") catch err {
        println("TotalTimeout:", errors.Is(err, task.ErrCancelled))
    }
    await ts.Close()
}
