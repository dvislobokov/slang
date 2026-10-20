// test: abrupt peer close: SetLinger(0) resets (ErrReset on read), writes to a closed peer fail with ErrReset (no SIGPIPE), Close wakes a blocked read and write with ErrClosed, Drop closes
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.net_reset

import std.io
import std

async func blockedRead(c net.Conn) string {
    var r = try await c.Read(10) catch err {
        return fmt.Format("blocked read: {0} {1}", errors.Is(err, net.ErrClosed), strings.HasPrefix(err.Message(), "read tcp 127.0.0.1:"))
    }
    return fmt.Format("read {0} bytes", len(r))
}

async func blockedWrite(c net.Conn) string {
    var chunk = strings.Bytes(strings.Repeat("w", 65536))
    for {
        try await c.Write(chunk) catch err {
            return fmt.Format("blocked write: {0}", errors.Is(err, net.ErrClosed))
        }
    }
    return "unreachable"
}

async func main() void! {
    var l = net.Listen("127.0.0.1:0")?

    // the peer resets the connection (linger 0): reads fail with ErrReset
    var c = await net.Dial(l.Addr())?
    var s = await l.Accept()?
    _ = await c.WriteString("unread data")?
    await task.Delay(10)
    s.SetLinger(0)?
    s.Close()
    try await c.Read(100) catch err {
        println("read after reset:", errors.Is(err, net.ErrReset), err.Message() == "read tcp " + c.RemoteAddr() + ": connection reset by peer")
    }
    c.Close()

    // writing to a peer that closed: EPIPE/ECONNRESET is ErrReset, the process survives (no SIGPIPE)
    var c2 = await net.Dial(l.Addr())?
    var s2 = await l.Accept()?
    s2.Close()
    var failed = false
    for _ in 0..200 {
        try await c2.WriteString("ping") catch err {
            failed = errors.Is(err, net.ErrReset)
            break
        }
        await task.Delay(1)
    }
    println("write to closed peer:", failed)
    c2.Close()

    // the peer closes normally while we are blocked reading: end of stream
    var c3 = await net.Dial(l.Addr())?
    var s3 = await l.Accept()?
    var pending = c3.Read(10)
    await task.Delay(10)
    s3.Close()
    println("peer closed during read:", len(await pending?))

    c3.Close()

    // Close wakes our own blocked read and write
    var c4 = await net.Dial(l.Addr())?
    var s4 = await l.Accept()?
    var r = blockedRead(c4)
    await task.Delay(10)
    c4.Close()
    println(await r)
    c4 = await net.Dial(l.Addr())?
    s4.Close()
    s4 = await l.Accept()?
    var w = blockedWrite(c4)
    await task.Delay(20)
    c4.Close()
    println(await w)
    s4.Close()

    // Drop closes: a connection that goes out of scope frees its socket,
    // and the peer reads end of stream
    var s5 = acceptAndForget(l)
    var c5 = await net.Dial(l.Addr())?
    await s5
    println("dropped peer:", len(await c5.Read(10)?))
    l.Close()
}

async func acceptAndForget(l net.Listener) {
    var s = try await l.Accept() catch err {
        return
    }
    _ = try await s.WriteString("") else 0
}
