// test: half-close: CloseWrite makes the peer read end of stream while the other direction keeps working; CloseRead; ReadAll/ReadExact
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.net_halfclose

import std
import std.io

async func main() void! {
    var l = net.Listen("127.0.0.1:0")?
    var c = await net.Dial(l.Addr())?
    var s = await l.Accept()?

    // client sends a request and half-closes; the server reads to the end,
    // then answers on the still-open direction
    _ = await c.WriteString("request body")?
    c.CloseWrite()?
    var req = await s.ReadAll()?
    println("server got:", strings.FromBytes(req))
    _ = await s.WriteString("response")?
    s.CloseWrite()?
    var resp = await c.ReadAll()?
    println("client got:", strings.FromBytes(resp))
    try await c.WriteString("more") catch err {
        println(
            "write after CloseWrite fails:",
            strings.HasPrefix(err.Message(), "write tcp 127.0.0.1:"),
        )
    }
    // ReadExact: exactly n bytes, or ErrEOF when the stream ends first
    var c2 = await net.Dial(l.Addr())?
    var s2 = await l.Accept()?
    _ = await c2.WriteString("0123456789")?
    c2.CloseWrite()?
    println(strings.FromBytes(await s2.ReadExact(4)?), strings.FromBytes(await s2.ReadExact(4)?))
    try await s2.ReadExact(4) catch err {
        println("short read:", errors.Is(err, net.ErrEOF))
    }
    println("after EOF Read:", len(await s2.Read(10)?), await s2.ReadInto(strings.Bytes("...."))?)
    // CloseRead: our reads end at once
    s2.CloseRead()?
    println("after CloseRead:", len(await s2.Read(10)?))
    c.Close()
    s.Close()
    c2.Close()
    s2.Close()
    l.Close()
}
