// test: 2000 concurrent TCP connections echo through one listener at once (sharded pollers, no lost wake-ups)
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.net_many_conns

import std
import std.io

async func serve(c net.Conn) void! {
    var buf = net.MakeBuffer(512)
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

async func client(addr string, id int) int! {
    var c = await net.Dial(addr)?
    var msg = fmt.Format("hello from client {0}\n", id)
    _ = await c.WriteString(msg)?
    var line = await c.ReadLine()?
    c.Close()
    if line + "\n" != msg {
        return error("bad echo: " + line)
    }
    return 1
}

async func main() void! {
    // compiler/tests/net_http.rs runs this with a larger count (up to 10000)
    var n = 2000
    var args = os.Args()
    if len(args) > 1 {
        n = strings.ToInt(args[1])?
    }
    var l = net.Listen("127.0.0.1:0")?
    var acc = acceptAll(l, n)
    var tasks []Task<int!> = []
    for i in 0..n {
        tasks = append(tasks, client(l.Addr(), i))
    }
    var ok = 0
    for t in tasks {
        ok += await t?
    }
    await acc?
    l.Close()
    println("echoed", ok == n, "connections")
}
