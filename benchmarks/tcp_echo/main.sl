namespace bench.tcp_echo

import std
import std.io

// 1000 concurrent TCP connections through loopback, each sending 1 MB in
// 64 KB writes and reading the echo back; the server echoes with
// ReadInto into a per-connection buffer (no allocation per read).

async func serve(c net.Conn) void! {
    var buf = net.MakeBuffer(65536)
    for {
        var n = await c.ReadInto(buf)?
        if n == 0 {
            break
        }
        _ = await c.Write(buf[..n])?
    }
    c.Close()
}

async func acceptAll(l net.Listener, n int) void! {
    for _ in 0..n {
        var c = await l.Accept()?
        _ = serve(c)
    }
}

async func send(c net.Conn, total int) void! {
    var chunk = strings.Bytes(strings.Repeat("c", 65536))
    var sent = 0
    for sent < total {
        _ = await c.Write(chunk)?
        sent += len(chunk)
    }
    c.CloseWrite()?
}

async func client(addr string, total int) int! {
    var c = await net.Dial(addr)?
    var writer = send(c, total)
    var buf = net.MakeBuffer(65536)
    var got = 0
    for {
        var n = await c.ReadInto(buf)?
        if n == 0 {
            break
        }
        got += n
    }
    await writer?
    c.Close()
    return got
}

async func main() void! {
    var conns = 1000
    var size = 1 << 20
    var l = net.Listen("127.0.0.1:0")?
    var acc = acceptAll(l, conns)
    var t0 = time.NowNanos()
    var tasks []Task<int!> = []
    for _ in 0..conns {
        tasks = append(tasks, client(l.Addr(), size))
    }
    var total = 0
    for t in tasks {
        total += await t?
    }
    await acc?
    var secs = float(time.NowNanos() - t0) / 1e9
    l.Close()
    io.Eprintln(
        fmt.Format(
            "tcp_echo slang: {0} MB echoed in {1} s ({2} MB/s)",
            total / size,
            secs,
            float(total / size) / secs,
        ),
    )
    println("echoed", total / size, "MB")
}
