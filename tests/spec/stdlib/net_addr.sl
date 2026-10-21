// test: net.IP and net.Addr: parse/format, IPv4/IPv6 predicates, zones, ParseAddr, service ports, Interfaces
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.net_addr

import std
import std.io

func show(s string) void! {
    var ip = net.ParseIP(s)?
    println(
        s,
        "->",
        ip.String(),
        "v4",
        ip.Is4(),
        "v6",
        ip.Is6(),
        "lo",
        ip.IsLoopback(),
        "priv",
        ip.IsPrivate(),
        "mc",
        ip.IsMulticast(),
        "ll",
        ip.IsLinkLocal(),
        "unspec",
        ip.IsUnspecified(),
    )
}

async func main() void! {
    for s in [
        "127.0.0.1",
        "10.1.2.3",
        "172.16.0.1",
        "172.32.0.1",
        "192.168.0.7",
        "8.8.8.8",
        "224.0.0.251",
        "169.254.1.1",
        "0.0.0.0",
        "::1",
        "::",
        "fd00::1",
        "fe80::1",
        "ff02::1",
        "2001:db8::1",
        "::ffff:10.0.0.1",
    ] {
        show(s)?
    }
    for bad in ["1.2.3", "256.1.1.1", "::g", "", "1.2.3.4%eth0", "[::1]"] {
        try net.ParseIP(bad) catch err {
            println(err)
        }
    }
    var m = net.ParseIP("::ffff:192.168.1.1")?
    println(
        m.To4().String(),
        len(m.Bytes()),
        len(m.To4().Bytes()),
        m.Equal(net.IPv4(192, 168, 1, 1)),
    )
    println(
        net.IPv4(1, 2, 3, 4).String(),
        net.IPv6Loopback().String(),
        net.IPv4Zero().String(),
        net.IPv6Zero().String(),
    )
    var zero net.IP
    println(zero.IsValid(), zero.String(), zero.IsLoopback())

    var a = net.ParseAddr("192.0.2.1:80")?
    println(a.String(), a.IP.String(), a.Port, a.Zone == "")
    var b = net.ParseAddr("[2001:db8::2]:443")?
    println(b.String(), b.Port)
    var z = net.ParseAddr("[fe80::1%1]:8080")?
    println(z.Zone, z.Port, z.IP.IsLinkLocal())
    for bad in ["192.0.2.1", "host:80", "[::1%x]:1", "1.2.3.4%1:80", ":-1"] {
        try net.ParseAddr(bad) catch err {
            println("bad:", bad)
        }
    }
    var r = await net.ResolveAddr("localhost:0")?
    println(r.String(), r.IP.IsLoopback())
    var any = await net.ResolveAddr(":9")?
    println(any.String())
    println(net.LookupPort("tcp", "8080")?)
    var ips = await net.Resolve("127.0.0.1")?
    println(len(ips), ips[0].String())
    var hosts = await net.LookupHost("localhost")?
    var hasLo = false
    for h in hosts {
        if h == "127.0.0.1" || h == "::1" {
            hasLo = true
        }
    }
    println("localhost resolves to a loopback address:", hasLo)

    var lo = false
    for it in net.Interfaces()? {
        if it.Loopback {
            for n in it.Addrs {
                if n.IP.String() == "127.0.0.1" {
                    lo = it.Up && n.Bits == 8
                }
            }
        }
    }
    println("loopback interface 127.0.0.1/8 up:", lo)
}
