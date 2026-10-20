// test: http.Transport proxies: absolute-form requests through an HTTP proxy (with Proxy-Authorization), https through a CONNECT tunnel, HTTP_PROXY / NO_PROXY from the environment
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.http_pool_proxy

import std.io
import std
import std.net

struct Log {
    mu task.Mutex
    lines []string
}

async func pipe(from net.Conn, to net.Conn) {
    for {
        var b = try await from.Read(16384) else []
        if len(b) == 0 {
            break
        }
        _ = try await to.Write(b) else 0
    }
    to.Close()
    from.Close()
}

// A tiny forward proxy: CONNECT tunnels, absolute-form GETs answered itself.
async func serveProxy(c net.Conn, log *Log) void! {
    var r = net.NewBufferedReader(c, 4096)
    var first = await r.ReadLine()?
    var auth = ""
    for {
        var h = await r.ReadLine()?
        if h == "" {
            break
        }
        if strings.HasPrefix(strings.ToLower(h), "proxy-authorization:") {
            auth = strings.Trim(h[20..])
        }
    }
    await log.mu.Lock()
    var line = strings.SplitN(first, " ", 3)
    var entry = line[0] + " " + line[1]
    if auth != "" {
        entry += " auth=" + auth
    }
    log.lines = append(log.lines, entry)
    log.mu.Unlock()
    if line[0] == "CONNECT" {
        var up = await net.Dial(line[1])?
        _ = await c.WriteString("HTTP/1.1 200 Connection Established\r\n\r\n")?
        var a = pipe(c, up)
        var b = pipe(up, c)
        await a
        await b
        return
    }
    var body = "proxied " + line[1]
    _ = await c.WriteString(fmt.Format("HTTP/1.1 200 OK\r\nContent-Length: {0}\r\nConnection: close\r\n\r\n{1}", len(body), body))?
    c.Close()
}

async func acceptLoop(l net.Listener, log *Log) {
    for {
        var c = try await l.Accept() catch err {
            return
        }
        _ = serveProxy(c, log)
    }
}

async func main() void! {
    var log = new Log{}
    var pl = net.Listen("127.0.0.1:0")?
    var proxying = acceptLoop(pl, log)
    var proxyURL = fmt.Format("http://127.0.0.1:{0}", pl.Port())

    var r = http.NewRouter()
    r.Get("/hello", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, "direct " + req.Proto)
    })
    var plain = http.test.NewServer(r)?
    var secure = http.test.NewTLSServer(r)?

    // plain http through the proxy (absolute form), with credentials
    var t1 = http.Transport{Proxy: "http://user:secret@" + proxyURL[7..]}
    var c1 = http.Client{Transport: &t1}
    var a = await c1.Get("http://example.invalid/path?q=1")?
    println("http via proxy:", a.Status, a.Body)

    // https through a CONNECT tunnel (then TLS and HTTP/2 inside it)
    var cfg = tls.Config{InsecureSkipVerify: true}
    var t2 = http.Transport{Proxy: proxyURL, TLS: &cfg}
    var c2 = http.Client{Transport: &t2}
    var b = await c2.Get(secure.URL + "/hello")?
    println("https via CONNECT:", b.Status, b.Body)
    var b2 = await c2.Get(secure.URL + "/hello")?
    println("again (pooled):", b2.Body, "dials", t2.Stats().Dials)

    // the environment
    os.Setenv("HTTP_PROXY", proxyURL)?
    os.Setenv("NO_PROXY", "")?
    var t3 = http.Transport{ProxyFromEnvironment: true}
    var c3 = http.Client{Transport: &t3}
    var e1 = await c3.Get("http://example.invalid/env")?
    println("HTTP_PROXY:", e1.Body)
    os.Setenv("NO_PROXY", "localhost,127.0.0.1")?
    var t4 = http.Transport{ProxyFromEnvironment: true}
    var c4 = http.Client{Transport: &t4}
    var e2 = await c4.Get(plain.URL + "/hello")?
    println("NO_PROXY:", e2.Body)
    os.Setenv("HTTP_PROXY", "")?

    await log.mu.Lock()
    for l in log.lines {
        println("proxy saw:", strings.Replace(l, secure.URL[8..], "<tls>"))
    }
    log.mu.Unlock()
    t2.CloseIdleConnections()
    await plain.Close()
    await secure.Close()
    pl.Close()
    await proxying
}
