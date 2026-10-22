// test: many concurrent requests on one HTTP/2 client connection: stream ids go out in increasing order (a lower id after a higher one is a PROTOCOL_ERROR that closes the connection)
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.http2_stream_order

import std
import std.io
import std.net

async func main() void! {
    var r = http.NewRouter()
    r.Get("/n/{i}", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, req.Param("i"))
    })
    var srv = http.Server{Addr: "127.0.0.1:0", Handler: r, H2C: true, MaxConcurrentStreams: 1000}
    srv.Listen()?
    var serving = srv.Serve()
    var t = http.Transport{H2C: true}
    var c = http.Client{Transport: &t}
    var ok = 0
    for round in 0..5 {
        var tasks []Task<http.Response!> = []
        for i in 0..60 {
            tasks = append(tasks, c.Get(srv.URL() + fmt.Format("/n/{0}", round * 100 + i)))
        }
        for i, tk in tasks {
            var res = await tk?
            if res.Body == fmt.Format("{0}", round * 100 + i) {
                ok += 1
            }
        }
    }
    var st = t.Stats()
    println("ok:", ok, "dials:", st.Dials, "http2 conns:", st.HTTP2)
    await srv.Shutdown()
    await serving?
}
