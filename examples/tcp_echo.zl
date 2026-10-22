namespace examples.tcp_echo

import std
import std.io

// A line-based TCP echo server and its clients in one program: a task per
// connection, buffered line reads, half-close, typed errors with errors.Is,
// and graceful shutdown by closing the listener (and waiting for the open
// connections: without it the last handler could still be running when
// main returns, which --leak-check reports).

async func handle(c net.Conn) void! {
    var r = net.NewBufferedReader(c, 0)
    var w = net.NewBufferedWriter(c, 0)
    for {
        var line = try await r.ReadLine() catch err {
            if errors.Is(err, net.ErrEOF) {
                break // the client half-closed: no more lines
            }
            return err
        }
        _ = await w.WriteString(strings.ToUpper(line) + "\n")?
        await w.Flush()?
    }
    c.Close()
}

// track runs one connection's handler and reports to the server's group.
async func track(c net.Conn, wg *task.WaitGroup) {
    try await handle(c) catch err {
        println("handler:", err)
    }
    wg.Done()
}

async func serve(l net.Listener) void! {
    var wg = new task.WaitGroup{}
    for {
        var c = try await l.Accept() catch err {
            if errors.Is(err, net.ErrClosed) {
                // the listener was closed: let the open connections finish
                await wg.Wait()
                return
            }
            return err
        }
        wg.Add(1)
        _ = track(c, wg)
    }
}

async func client(addr string, name string) string! {
    var c = await net.Dial(addr)?
    _ = await c.WriteString("hello from " + name + "\nbye from " + name + "\n")?
    c.CloseWrite()? // half-close: we are done sending
    var answer = strings.Trim(strings.FromBytes(await c.ReadAll()?))
    c.Close()
    return strings.Replace(answer, "\n", " | ")
}

async func main() void! {
    var l = net.Listen("127.0.0.1:0")?
    var server = serve(l)
    var addr = l.Addr()

    var a = client(addr, "alice")
    var b = client(addr, "bob")
    println(await a?)
    println(await b?)

    // a timeout: the server never answers an empty request
    var c = await net.Dial(addr)?
    c.SetTimeout(50)
    try await c.ReadLine() catch err {
        println("timeout:", net.IsTimeout(err))
    }
    c.Close()

    l.Close()
    await server?

    try await net.Dial(addr) catch err {
        println("after shutdown:", errors.Is(err, net.ErrRefused))
    }
}
