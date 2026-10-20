namespace bench.http_hello

import std.io
import std
import std.net

// HTTP/1.1 "hello" throughput: the std.net.http server answering a
// keep-alive load generator (CONNS connections, REQUESTS requests in all,
// one request in flight per connection, like wrk -c CONNS).
//   main                  server and load generator in one process
//   main --server         only the server: prints its port (stderr), serves until killed
//   main --load host:port only the load generator (against any server)
//   main --server-tls     HTTP/2 over TLS (std.net.http.test certificate, ALPN h2):
//                         prints its port (stderr), serves until killed

async func Hello(req *http.Request, res *http.Response) void! {
    res.Text(200, "hello, world\n")
}

func contentLength(head string) int {
    for line in strings.Split(head, "\r\n") {
        if strings.HasPrefix(strings.ToLower(line), "content-length:") {
            return try strings.ToInt(strings.Trim(line[15..])) else -1
        }
    }
    return -1
}

async func worker(addr string, n int) int! {
    var c = await net.Dial(addr)?
    var r = net.NewBufferedReader(c, 16384)
    var req = "GET / HTTP/1.1\r\nHost: bench\r\nUser-Agent: slang-load\r\n\r\n"
    var done = 0
    for done < n {
        _ = await c.WriteString(req)?
        var head = ""
        for {
            var line = await r.ReadLine()?
            if line == "" {
                break
            }
            head += line + "\r\n"
        }
        var body = contentLength(head)
        if body < 0 || !strings.HasPrefix(head, "HTTP/1.1 200") {
            return error("bad response: " + head)
        }
        _ = await r.ReadExact(body)?
        done += 1
    }
    c.Close()
    return done
}

async func load(addr string, conns int, total int) void! {
    // warm-up
    _ = await worker(addr, 100)?
    var t0 = time.NowNanos()
    var tasks []Task<int!> = []
    for _ in 0..conns {
        tasks = append(tasks, worker(addr, total / conns))
    }
    var n = 0
    for t in tasks {
        n += await t?
    }
    var secs = float(time.NowNanos() - t0) / 1e9
    io.Eprintln(fmt.Format("http_hello slang load: {0} requests over {1} connections in {2} s: {3} req/s", n, conns, secs, float(n) / secs))
    println("requests", n)
}

async func main() void! {
    var args = os.Args()
    var conns = 64
    var total = 200000
    if os.Getenv("BENCH_REQUESTS") != "" {
        total = strings.ToInt(os.Getenv("BENCH_REQUESTS"))?
    }
    if len(args) > 2 && args[1] == "--load" {
        await load(args[2], conns, total)?
        return
    }
    var r = http.NewRouter()
    r.Get("/", Hello)
    if len(args) > 1 && args[1] == "--server-tls" {
        var ts = http.test.NewTLSServer(r)?
        io.Eprintln(fmt.Format("{0}", ts.Port()))
        for {
            await task.Delay(1000000)
        }
    }
    var srv = http.Server{Addr: "127.0.0.1:0", Handler: r}
    srv.Listen()?
    var serving = srv.Serve()
    if len(args) > 1 && args[1] == "--server" {
        io.Eprintln(fmt.Format("{0}", srv.Port()))   // stderr: unbuffered
        await serving?
        return
    }
    await load(fmt.Format("127.0.0.1:{0}", srv.Port()), conns, total)?
    await srv.Shutdown()
    await serving?
}
