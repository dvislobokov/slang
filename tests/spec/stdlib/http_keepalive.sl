// test: the http server keeps HTTP/1.1 connections alive, sends a Date header, serves concurrent clients, and Shutdown closes idle connections
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.http_keepalive

import std.io
import std
import std.net

struct Hits {
    Mu task.Mutex
    N int
}

struct Counter {
    hits *Hits

    func Serve(req *http.Request, res *http.Response) Task<void!> {
        return Count(this.hits, req, res)
    }
}

async func Count(h *Hits, req *http.Request, res *http.Response) void! {
    await h.Mu.Lock()
    h.N += 1
    h.Mu.Unlock()
    res.Text(200, req.Method + " " + req.Path + " body=" + req.Body)
}

// The Date header changes every second: keep only its shape.
func noDate(s string) string {
    var i = strings.Index(s, "Date: ")
    if i < 0 {
        return s
    }
    var rest = s[i..]
    var j = strings.Index(rest, "\r\n")
    var d = rest[6..j]
    var ok = strings.HasSuffix(d, " GMT") && len(d) == 29
    return s[..i] + "Date: <" + fmt.Format("{0}", ok) + ">" + rest[j..]
}

async func main() void! {
    var hits = new Hits{}
    var srv = http.Server{Addr: "127.0.0.1:0", Handler: new Counter{hits: hits}}
    srv.Listen()?
    var serving = srv.Serve()
    var c = await net.Dial(fmt.Format("127.0.0.1:{0}", srv.Port()))?
    _ = await c.WriteString("POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc")?
    println(strings.Replace(noDate(await c.ReadString(1000)?), "\r\n", "|"))
    _ = await c.WriteString("GET /b HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n")?
    println(strings.Replace(noDate(await c.ReadString(1000)?), "\r\n", "|"))
    var base = fmt.Format("http://127.0.0.1:{0}", srv.Port())
    var (r1, r2, r3) = await task.All(http.Get(base + "/1"), http.Get(base + "/2"), http.Get(base + "/3"))?
    println(r1.Body, "|", r2.Body, "|", r3.Body)
    await srv.Shutdown()
    await serving?
    var rest = await c.ReadString(10)?
    println("idle connection closed:", len(rest) == 0, "hits", hits.N)
    c.Close()
}
