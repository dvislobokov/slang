namespace std.net

// The Slang-source std.net (see sema/stdsrc.rs for the rules): TCP and
// UDP over non-blocking sockets. Blocking points are awaits on readiness
// waits of the runtime's I/O poller (rtWaitReadable/rtWaitWritable), so a
// task waiting for the network never blocks a worker thread.

// Host and port of an address.
struct HostPort {
    Host string
    Port int
}

// SplitHostPort splits "host:port", "[ipv6]:port" or ":port".
func SplitHostPort(addr string) HostPort! {
    var i = std.strings.LastIndex(addr, ":")
    if i < 0 {
        return error("address " + addr + ": missing port")
    }
    var host = addr[..i]
    var ps = addr[i + 1..]
    if std.strings.HasPrefix(host, "[") && std.strings.HasSuffix(host, "]") {
        host = host[1..len(host) - 1]
    } else if std.strings.Contains(host, ":") {
        return error("address " + addr + ": too many colons (write IPv6 addresses as [ip]:port)")
    }
    var port = try std.strings.ToInt(ps) else -1
    if port < 0 || port > 65535 {
        return error("address " + addr + ": invalid port " + ps)
    }
    return HostPort{Host: host, Port: port}
}

// JoinHostPort is the inverse of SplitHostPort.
func JoinHostPort(host string, port int) string {
    if std.strings.Contains(host, ":") {
        return std.fmt.Format("[{0}]:{1}", host, port)
    }
    return std.fmt.Format("{0}:{1}", host, port)
}

// ResolveIP looks up the IP addresses of host (IPv4 first); an IP literal
// resolves to itself. The lookup runs on a helper thread.
async func ResolveIP(host string) []string! {
    if host == "" || host == "localhost" {
        return ["127.0.0.1"]
    }
    var h = await rtResolve(host)
    return rtResolveTake(h)?
}

// Conn is a TCP connection. Copies share the socket, which is closed by
// Close() or when the last copy is gone.
struct Conn {
    s *sock

    // Reads up to n bytes; an empty result means the peer closed the
    // connection (end of stream).
    func Read(n int) Task<[]byte!> {
        return this.s.read(n)
    }

    // Read as a string.
    func ReadString(n int) Task<string!> {
        return this.s.readString(n)
    }

    // Writes all of data; yields the number of bytes written.
    func Write(data []byte) Task<int!> {
        return this.s.write(data)
    }

    func WriteString(text string) Task<int!> {
        return this.s.writeString(text)
    }

    // Timeout in milliseconds for each later Read/Write (0: none); a
    // timed-out operation fails with "i/o timeout".
    func SetTimeout(ms int) {
        this.s.timeout = ms
    }

    func Close() {
        this.s.close()
    }

    func IsClosed() bool {
        return this.s == nil || this.s.fd < 0
    }

    func LocalAddr() string {
        return rtLocalAddr(this.s.fd)
    }

    func RemoteAddr() string {
        return rtRemoteAddr(this.s.fd)
    }
}

// A socket owned by Conn/Listener/UDPConn; closed by Drop at the latest.
struct sock {
    fd int
    timeout int

    func Drop() {
        if this.fd >= 0 {
            rtClose(this.fd)
        }
    }

    func close() {
        if this.fd >= 0 {
            var fd = this.fd
            this.fd = -1
            rtClose(fd)
        }
    }

    func closedErr() error {
        return error("use of closed network connection")
    }

    async func waitRead() void! {
        var r = await rtWaitReadable(this.fd, this.timeout)
        if r == 1 {
            return error("read: i/o timeout")
        }
        if r == 2 {
            return this.closedErr()
        }
        if r == 3 {
            return std.task.ErrCancelled
        }
    }

    async func waitWrite() void! {
        var r = await rtWaitWritable(this.fd, this.timeout)
        if r == 1 {
            return error("write: i/o timeout")
        }
        if r == 2 {
            return this.closedErr()
        }
        if r == 3 {
            return std.task.ErrCancelled
        }
    }

    async func read(n int) []byte! {
        for {
            if this.fd < 0 {
                return this.closedErr()
            }
            var b = rtRead(this.fd, n)?
            if len(b) > 0 || !rtAgain() {
                return b
            }
            await this.waitRead()?
        }
    }

    async func readString(n int) string! {
        var b = await this.read(n)?
        return std.strings.FromBytes(b)
    }

    async func write(data []byte) int! {
        var off = 0
        for off < len(data) {
            if this.fd < 0 {
                return this.closedErr()
            }
            var k = rtWrite(this.fd, data, off)?
            if k < 0 {
                await this.waitWrite()?
            } else {
                off += k
            }
        }
        return off
    }

    async func writeString(text string) int! {
        var off = 0
        for off < len(text) {
            if this.fd < 0 {
                return this.closedErr()
            }
            var k = rtWriteStr(this.fd, text, off)?
            if k < 0 {
                await this.waitWrite()?
            } else {
                off += k
            }
        }
        return off
    }
}

// Dial connects to addr ("host:port") over TCP.
async func Dial(addr string) Conn! {
    return await DialTimeout(addr, 0)?
}

// DialTimeout is Dial with a connect timeout in milliseconds (0: none).
async func DialTimeout(addr string, ms int) Conn! {
    var hp = SplitHostPort(addr)?
    var ips = await ResolveIP(hp.Host)?
    var last = error("dial " + addr + ": no address")
    for ip in ips {
        var fd = try rtConnect(ip, hp.Port) catch err {
            last = err
            continue
        }
        var s = new sock{fd: fd, timeout: ms}
        var r = await rtWaitWritable(fd, ms)
        if r == 1 {
            return error("dial " + addr + ": i/o timeout")
        }
        if r == 3 {
            return std.task.ErrCancelled
        }
        try rtConnectResult(fd) catch err {
            last = std.errors.Wrap(err, "dial tcp " + addr)
            continue
        }
        s.timeout = 0
        return Conn{s: s}
    }
    return last
}

// Listener accepts TCP connections.
struct Listener {
    s *sock

    // Waits for the next connection; fails once the listener is closed.
    func Accept() Task<Conn!> {
        return acceptOn(this.s)
    }

    func Close() {
        this.s.close()
    }

    // The bound address ("ip:port").
    func Addr() string {
        return rtLocalAddr(this.s.fd)
    }

    // The bound port (useful after listening on port 0).
    func Port() int {
        return rtPort(this.s.fd)
    }
}

async func acceptOn(s *sock) Conn! {
    for {
        if s.fd < 0 {
            return error("accept: listener closed")
        }
        var fd = rtAccept(s.fd)?
        if fd >= 0 {
            return Conn{s: new sock{fd: fd}}
        }
        var r = await rtWaitReadable(s.fd, 0)
        if r == 2 {
            return error("accept: listener closed")
        }
        if r == 3 {
            return std.task.ErrCancelled
        }
    }
}

func listenIP(host string) string {
    if host == "" {
        return "0.0.0.0"
    }
    if host == "localhost" {
        return "127.0.0.1"
    }
    return host
}

// Listen listens for TCP connections on addr ("host:port"; an empty host
// means every interface, port 0 picks a free port). The host must be an IP
// address or localhost.
func Listen(addr string) Listener! {
    var hp = SplitHostPort(addr)?
    var fd = rtListen(listenIP(hp.Host), hp.Port)?
    return Listener{s: new sock{fd: fd}}
}

// A received datagram.
struct Packet {
    Data []byte
    Addr string
}

// UDPConn is a bound UDP socket.
struct UDPConn {
    s *sock

    // Waits for a datagram of at most n bytes.
    func RecvFrom(n int) Task<Packet!> {
        return recvFromOn(this.s, n)
    }

    // Sends data to addr ("ip:port").
    func SendTo(data []byte, addr string) Task<int!> {
        return sendToOn(this.s, data, addr)
    }

    func SetTimeout(ms int) {
        this.s.timeout = ms
    }

    func Close() {
        this.s.close()
    }

    func Addr() string {
        return rtLocalAddr(this.s.fd)
    }

    func Port() int {
        return rtPort(this.s.fd)
    }
}

async func recvFromOn(s *sock, n int) Packet! {
    for {
        if s.fd < 0 {
            return s.closedErr()
        }
        var b = rtRecvFrom(s.fd, n)?
        if !rtAgain() {
            return Packet{Data: b, Addr: JoinHostPort(rtLastFrom(), rtLastFromPort())}
        }
        await s.waitRead()?
    }
}

async func sendToOn(s *sock, data []byte, addr string) int! {
    var hp = SplitHostPort(addr)?
    var ips = await ResolveIP(hp.Host)?
    for {
        if s.fd < 0 {
            return s.closedErr()
        }
        var k = rtSendTo(s.fd, data, ips[0], hp.Port)?
        if k >= 0 {
            return k
        }
        await s.waitWrite()?
    }
}

// ListenUDP binds a UDP socket to addr ("ip:port", port 0 = any).
func ListenUDP(addr string) UDPConn! {
    var hp = SplitHostPort(addr)?
    var fd = rtUdpBind(listenIP(hp.Host), hp.Port)?
    return UDPConn{s: new sock{fd: fd}}
}
