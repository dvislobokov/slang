// test: dialing a free loopback port from that same port (a TCP self-connect) is refused, not a connection to itself
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.net_self_connect

import std
import std.io

async func main() void! {
    // a free port: listen on :0, note the address, close
    var l = net.Listen("127.0.0.1:0")?
    var addr = l.Addr()
    l.Close()
    // the kernel may pick the dialled port as the source port when nothing
    // listens there (likely under load); forced here with LocalAddr
    var d = net.Dialer{LocalAddr: addr}
    try await d.Dial(addr) catch err {
        println("refused:", errors.Is(err, net.ErrRefused))
        return
    }
    println("connected to itself")
}
