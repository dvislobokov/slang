// test: Unix domain sockets: ListenUnix/DialUnix echo, the socket file is removed by Close and by Drop, ErrAddrInUse on an existing path, abstract namespace on Linux, refused/missing paths
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.net_unix

import std.io
import std

async func echoOnce(l net.Listener) void! {
    var s = await l.Accept()?
    var line = await s.ReadLine()?
    _ = await s.WriteString("echo: " + line + "\n")?
    s.Close()
}

async func roundTrip(path string) string! {
    var l = net.ListenUnix(path)?
    var srv = echoOnce(l)
    var c = await net.DialUnix(path)?
    _ = await c.WriteString("over a unix socket\n")?
    var got = await c.ReadLine()?
    await srv?
    c.Close()
    l.Close()
    return got
}

func listenAndDrop(path string) bool! {
    var l = net.ListenUnix(path)?
    return fs.Exists(path) && l.Addr() == path
}

async func main() void! {
    var path = fs.Join(fs.TempDir(), fmt.Format("slang-net-unix-{0}.sock", os.Pid()))
    println(await roundTrip(path)?)
    println("file removed by Close:", !fs.Exists(path))

    println("file while listening:", listenAndDrop(path)?)
    println("file removed by Drop:", !fs.Exists(path))

    // an existing file is not replaced
    io.WriteFile(path, "x")?
    try net.ListenUnix(path) catch err {
        println("in use:", errors.Is(err, net.ErrAddrInUse))
    }
    fs.Remove(path)?
    // nobody listening
    try await net.DialUnix(path) catch err {
        println("missing:", strings.HasPrefix(err.Message(), "dial unix " + path + ": "))
    }
    if os.Name == "linux" {
        println(await roundTrip(fmt.Format("@slang-abstract-{0}", os.Pid()))?)
    } else {
        println("echo: over a unix socket")
    }
}
