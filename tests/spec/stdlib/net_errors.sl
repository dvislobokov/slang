// test: std.net errors: sentinels through OpError wraps (errors.Is / errors.As), refused, closed, address in use, unknown hosts, bad addresses
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.net_errors

import std.io
import std

async func main() void! {
    var hp = net.SplitHostPort("[::1]:8080")?
    println(hp.Host, hp.Port, net.JoinHostPort(hp.Host, hp.Port), net.JoinHostPort("example.org", 80))
    try net.SplitHostPort("nohost") catch err {
        println(err)
    }
    try net.SplitHostPort("a:99999") catch err {
        println(err)
    }
    println(await net.ResolveIP("10.1.2.3")?, await net.ResolveIP("localhost")?)

    // Accept on a closed listener: ErrClosed inside an OpError
    var l = net.Listen("127.0.0.1:0")?
    var port = l.Port()
    var addr = l.Addr()
    l.Close()
    l.Close()
    try await l.Accept() catch err {
        println("accept:", errors.Is(err, net.ErrClosed), err.Message() == "accept tcp " + addr + ": use of closed network connection")
        var oe net.OpError
        println("as OpError:", errors.As(err, &oe), oe.Op, oe.Net, oe.Addr == addr, errors.Is(oe.Err, net.ErrClosed))
    }

    // refused: Go's message format, the sentinel through the wrap
    try await net.Dial(fmt.Format("127.0.0.1:{0}", port)) catch err {
        println("refused:", errors.Is(err, net.ErrRefused), err.Message() == fmt.Format("dial tcp 127.0.0.1:{0}: connection refused", port))
        println("not a timeout:", net.IsTimeout(err), errors.Is(err, net.ErrReset))
    }

    // address in use (SO_REUSEADDR does not let two listeners share a port)
    var a = net.Listen("127.0.0.1:0")?
    try net.Listen(a.Addr()) catch err {
        println("in use:", errors.Is(err, net.ErrAddrInUse), strings.HasPrefix(err.Message(), "listen tcp 127.0.0.1:"))
    }
    a.Close()

    // host names are resolved for Listen; unknown names wrap ErrNotFound
    var lh = net.Listen("localhost:0")?
    println("listen localhost:", strings.HasPrefix(lh.Addr(), "127.0.0.1:"))
    lh.Close()
    try net.Listen("nosuchhost.invalid:80") catch err {
        println("listen unknown host:", errors.Is(err, net.ErrNotFound))
    }
    try await net.Dial("nosuchhost.invalid:80") catch err {
        println("dial unknown host:", errors.Is(err, net.ErrNotFound), strings.HasPrefix(err.Message(), "dial tcp nosuchhost.invalid:80: lookup nosuchhost.invalid"))
    }
    try await net.Resolve("nosuchhost.invalid") catch err {
        println("resolve unknown host:", errors.Is(err, net.ErrNotFound))
    }
    try net.Listen("127.0.0.1:http-nope") catch err {
        println(err)
    }
    try net.ParseAddr("[fe80::1]x") catch err {
        println(err)
    }
}
