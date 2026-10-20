// test: the http client decodes chunked bodies, combines repeated headers, rejects https, and times out with Client{Timeout}
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.http_client_raw

import std.io
import std
import std.net

async func Raw(l net.Listener, reply string) void! {
    var c = await l.Accept()?
    var req = await c.ReadString(4096)?
    println("request:", strings.Split(req, "\r\n")[0])
    if reply != "" {
        _ = await c.WriteString(reply)?
    } else {
        await task.Delay(200)
    }
    c.Close()
}

async func main() void! {
    var l = net.Listen("127.0.0.1:0")?
    var url = fmt.Format("http://localhost:{0}/data?x=1", l.Port())
    var s = Raw(l, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-A: 1\r\nx-a: 2\r\n\r\n4\r\nWiki\r\n6;note=x\r\npedia \r\nE\r\nin chunks, ok.\r\n0\r\nTrailer: t\r\n\r\n")
    var res = await http.Get(url)?
    await s?
    println(res.Status, res.Header("X-A"), "[" + res.Body + "]")

    var s2 = Raw(l, "")
    var client = http.Client{Timeout: 30}
    try await client.Get(url) catch err {
        println("timeout:", strings.HasSuffix(err.Message(), "i/o timeout"))
    }
    await s2?
    l.Close()

    try await http.Get("https://example.org/") catch err {
        println(err)
    }
    try await http.Get("ftp://example.org/") catch err {
        println(err)
    }
    var u = http.ParseURL("http://[::1]:8080/a/b?q=1#frag")?
    println(u.Hostname, u.Port, u.Path, u.RawQuery, u.RequestURI())
}
