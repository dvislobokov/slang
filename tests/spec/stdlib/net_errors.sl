// test: std.net errors: bad addresses, refused connections, Accept after Close, SplitHostPort/JoinHostPort, ResolveIP of literals
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
    var l = net.Listen("127.0.0.1:0")?
    var port = l.Port()
    l.Close()
    try await l.Accept() catch err {
        println(err)
    }
    try await net.Dial(fmt.Format("127.0.0.1:{0}", port)) catch err {
        println("refused:", strings.Contains(err.Message(), "connect"))
    }
    try net.Listen("example.org:80") catch err {
        println("listen needs an IP:", strings.HasPrefix(err.Message(), "listen tcp example.org:80"))
    }
}
