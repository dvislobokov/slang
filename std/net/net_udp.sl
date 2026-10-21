namespace std.net

// std.net, second file (see net.sl for the overview): UDP sockets,
// buffered reads and writes over a Conn, and network interfaces.

// ---- UDP ----

// A received datagram (RecvFrom).
struct Packet {
    Data []byte
    Addr string
}

// The result of UDPConn.ReadFrom: N bytes were stored; Truncated is set
// when the datagram was longer than the buffer (the rest is dropped).
struct Datagram {
    N         int
    From      Addr
    Truncated bool
}

// UDPConn is a UDP socket: bound (ListenUDP: RecvFrom/SendTo,
// ReadFrom/WriteTo) or connected (DialUDP: Read/Write as well).
struct UDPConn {
    s *sock

    // Waits for a datagram of at most n bytes (a longer one is truncated).
    func RecvFrom(n int) Task<Packet!> {
        return recvFromOn(this.s, n)
    }

    // Sends data to addr ("host:port").
    func SendTo(data []byte, addr string) Task<int!> {
        return sendToOn(this.s, data, addr)
    }

    // Receives one datagram into buf (no allocation).
    func ReadFrom(buf []byte) Task<Datagram!> {
        return readFromOn(this.s, buf)
    }

    // Sends buf to `to`.
    func WriteTo(buf []byte, to Addr) Task<int!> {
        return writeToOn(this.s, buf, to.host(), to.Port)
    }

    // Connected sockets: the next datagram (up to n bytes) / into buf.
    func Read(n int) Task<[]byte!> {
        return this.s.read(n)
    }

    func ReadInto(buf []byte) Task<int!> {
        return udpReadInto(this.s, buf)
    }

    // Connected sockets: sends one datagram.
    func Write(data []byte) Task<int!> {
        return writeToOn(this.s, data, "", 0)
    }

    func SetTimeout(ms int) {
        this.s.timeout = ms
    }

    func SetReadDeadline(t int) {
        this.s.rdl = t
    }

    func SetWriteDeadline(t int) {
        this.s.wdl = t
    }

    func Close() {
        if this.s != nil {
            this.s.close()
        }
    }

    func IsClosed() bool {
        return this.s == nil || this.s.isClosed()
    }

    func Addr() string {
        return this.s.laddr
    }

    func Local() Addr {
        return addrOf(this.s.laddr)
    }

    // The peer of a connected socket.
    func Remote() Addr {
        return addrOf(rtRemoteAddr(this.s.h))
    }

    func Port() int {
        return rtPort(this.s.h)
    }

    func Fd() int {
        return rtFd(this.s.h)
    }

    // SO_BROADCAST: allow sending to broadcast addresses.
    func SetBroadcast(on bool) void! {
        this.s.setFlag(11, on)?
    }

    // Joins / leaves multicast group `group` on interface ifname ("" =
    // the system's choice).
    func JoinGroup(group string, ifname string) void! {
        try rtMcast(this.s.h, true, group, ifname) catch err {
            return this.s.fail("join", err)
        }
    }

    func LeaveGroup(group string, ifname string) void! {
        try rtMcast(this.s.h, false, group, ifname) catch err {
            return this.s.fail("leave", err)
        }
    }

    // Whether this host receives its own multicast datagrams (default on).
    func SetMulticastLoopback(on bool) void! {
        this.s.setFlag(15, on)?
    }

    // The interface multicast datagrams are sent from ("" = the system's
    // choice by route; "lo" for loopback-only groups).
    func SetMulticastInterface(ifname string) void! {
        var idx = 0
        if ifname != "" {
            idx = rtIfIndex(ifname)
            if idx == 0 {
                return this.s.fail("set", error("no such network interface " + ifname))
            }
        }
        this.s.setOpt(17, idx)?
    }

    func SetMulticastTTL(ttl int) void! {
        this.s.setOpt(16, ttl)?
    }

    func SetReadBuffer(n int) void! {
        this.s.setOpt(6, n)?
    }

    func SetWriteBuffer(n int) void! {
        this.s.setOpt(7, n)?
    }
}

async func recvFromOn(s *sock, n int) Packet! {
    s.checkDeadline(s.rdl, "read")?
    for {
        var b = try rtRecvFrom(s.h, n) catch err {
            return s.fail("read", err)
        }
        if !rtAgain() {
            return Packet{Data: b, Addr: rtLastFrom()}
        }
        await s.waitRead("read")?
    }
}

async func readFromOn(s *sock, buf []byte) Datagram! {
    s.checkDeadline(s.rdl, "read")?
    for {
        var k = try rtReadFrom(s.h, buf) catch err {
            return s.fail("read", err)
        }
        if k >= 0 {
            return Datagram{N: k, From: addrOf(rtLastFrom()), Truncated: rtLastTrunc()}
        }
        await s.waitRead("read")?
    }
}

async func udpReadInto(s *sock, buf []byte) int! {
    var d = await readFromOn(s, buf)?
    return d.N
}

async func sendToOn(s *sock, data []byte, addr string) int! {
    var hp = try SplitHostPort(addr) catch err {
        return s.fail("write", err)
    }
    var host = hp.Host
    if !rtIsIP(host) && !std.strings.Contains(host, "%") {
        var ips = try await ResolveIP(host) catch err {
            return s.fail("write", err)
        }
        host = ips[0]
    }
    return await writeToOn(s, data, host, hp.Port)?
}

async func writeToOn(s *sock, data []byte, ip string, port int) int! {
    s.checkDeadline(s.wdl, "write")?
    for {
        var k = try rtSendTo(s.h, data, ip, port) catch err {
            if ip != "" {
                return opError("write", s.nw, JoinHostPort(ip, port), err)
            }
            return s.fail("write", err)
        }
        if k >= 0 {
            return k
        }
        await s.waitWrite("write")?
    }
}

// ListenUDP binds a UDP socket to addr ("host:port", port 0 = any).
func ListenUDP(addr string) UDPConn! {
    var lc ListenConfig
    return lc.ListenUDP(addr)?
}

// DialUDP makes a UDP socket connected to addr ("host:port"): Read/Write
// exchange datagrams with that peer only.
async func DialUDP(addr string) UDPConn! {
    var hp = try SplitHostPort(addr) catch err {
        return opError("dial", "udp", addr, err)
    }
    var host = hp.Host
    if host == "" {
        host = "127.0.0.1"
    }
    if !rtIsIP(host) && !std.strings.Contains(host, "%") {
        var ips = try await ResolveIP(host) catch err {
            return opError("dial", "udp", addr, err)
        }
        host = ips[0]
    }
    var h = try rtUdpDial(host, hp.Port, "", 0) catch err {
        return opError("dial", "udp", addr, err)
    }
    return UDPConn{s: new sock{h: h, nw: "udp", laddr: rtLocalAddr(h), peerOK: true}}
}

// ---- buffered I/O ----

struct rbuf {
    c   Conn
    buf []byte
    r   int
    w   int

    async func fill() int! {
        if this.r > 0 {
            if this.r < this.w {
                rtCopyBytes(this.buf, 0, this.buf, this.r, this.w - this.r)
            }
            this.w -= this.r
            this.r = 0
        }
        if this.w == len(this.buf) {
            return 0
        }
        var k = await this.c.s.readInto(this.buf, this.w)?
        this.w += k
        return k
    }

    async func readByte() byte! {
        if this.r == this.w {
            var k = await this.fill()?
            if k == 0 {
                return this.c.s.fail("read", ErrEOF)
            }
        }
        var b = this.buf[this.r]
        this.r += 1
        return b
    }

    // Up to and including delim; at the end of the stream the rest
    // (ErrEOF if nothing is left).
    async func readUntil(delim byte) []byte! {
        var out []byte = []
        for {
            var i = rtIndexByte(this.buf[..this.w], this.r, int(delim))
            if i >= 0 {
                out = rtJoin(out, this.buf[this.r..i + 1], i + 1 - this.r)
                this.r = i + 1
                return out
            }
            out = rtJoin(out, this.buf[this.r..this.w], this.w - this.r)
            this.r = this.w
            var k = await this.fill()?
            if k == 0 {
                if len(out) == 0 {
                    return this.c.s.fail("read", ErrEOF)
                }
                return out
            }
        }
        return out
    }

    // A line straight from the buffer when it is all there (the common
    // case: one copy, no intermediate slice); longer lines accumulate.
    async func readLine() string! {
        for {
            var i = rtIndexByte(this.buf[..this.w], this.r, 10)
            if i >= 0 {
                var end = i
                if end > this.r && this.buf[end - 1] == 13 {
                    end -= 1
                }
                var line = std.strings.FromBytes(this.buf[this.r..end])
                this.r = i + 1
                return line
            }
            if this.r == 0 && this.w == len(this.buf) {
                return await this.longLine()?
            }
            var k = await this.fill()?
            if k == 0 {
                if this.r == this.w {
                    return this.c.s.fail("read", ErrEOF)
                }
                var rest = std.strings.FromBytes(this.buf[this.r..this.w])
                this.r = this.w
                return rest
            }
        }
        return ""
    }

    // A line longer than the buffer.
    async func longLine() string! {
        var b = await this.readUntil(10)?
        var n = len(b)
        if n > 0 && b[n - 1] == 10 {
            n -= 1
            if n > 0 && b[n - 1] == 13 {
                n -= 1
            }
        }
        return std.strings.FromBytes(b[..n])
    }

    async func readInto(p []byte) int! {
        if len(p) == 0 {
            return 0
        }
        if this.r == this.w {
            if len(p) >= len(this.buf) {
                return await this.c.s.readInto(p, 0)?
            }
            var k = await this.fill()?
            if k == 0 {
                return 0
            }
        }
        var n = rtCopyBytes(p, 0, this.buf, this.r, this.w - this.r)
        this.r += n
        return n
    }

    async func readExact(n int) []byte! {
        var out = rtBytes(n)
        var off = 0
        for off < n {
            if this.r == this.w {
                var k = await this.fill()?
                if k == 0 {
                    return this.c.s.fail("read", ErrEOF)
                }
            }
            var m = rtCopyBytes(out, off, this.buf, this.r, this.w - this.r)
            this.r += m
            off += m
        }
        return out
    }
}

// BufferedReader reads a Conn through a buffer (bufio-style). Copies share
// the buffer.
struct BufferedReader {
    b *rbuf

    func ReadByte() Task<byte!> {
        return this.b.readByte()
    }

    // One line without "\n" / "\r\n"; ErrEOF at the end of the stream.
    func ReadLine() Task<string!> {
        return this.b.readLine()
    }

    // Up to and including delim (the rest at the end of the stream).
    func ReadUntil(delim byte) Task<[]byte!> {
        return this.b.readUntil(delim)
    }

    // Into p: bytes read, 0 at the end of the stream.
    func ReadInto(p []byte) Task<int!> {
        return this.b.readInto(p)
    }

    // Exactly n bytes (ErrEOF if the stream ends first).
    func ReadExact(n int) Task<[]byte!> {
        return this.b.readExact(n)
    }

    // Bytes buffered and not read yet.
    func Buffered() int {
        return this.b.w - this.b.r
    }
}

// NewBufferedReader buffers reads of c (size <= 0: 4096 bytes).
func NewBufferedReader(c Conn, size int) BufferedReader {
    var n = size
    if n <= 0 {
        n = 4096
    }
    return BufferedReader{b: new rbuf{c: c, buf: rtBytes(n)}}
}

struct wbuf {
    c   Conn
    buf []byte
    n   int

    async func flush() void! {
        if this.n > 0 {
            _ = await this.c.s.write(this.buf[..this.n])?
            this.n = 0
        }
    }

    async func write(p []byte) int! {
        if this.n + len(p) > len(this.buf) {
            await this.flush()?
            if len(p) >= len(this.buf) {
                return await this.c.s.write(p)?
            }
        }
        this.n += rtCopyBytes(this.buf, this.n, p, 0, len(p))
        return len(p)
    }

    async func writeString(s string) int! {
        return await this.write(std.strings.Bytes(s))?
    }
}

// BufferedWriter collects small writes to a Conn; Flush sends them.
// Copies share the buffer.
struct BufferedWriter {
    b *wbuf

    func Write(p []byte) Task<int!> {
        return this.b.write(p)
    }

    func WriteString(s string) Task<int!> {
        return this.b.writeString(s)
    }

    func Flush() Task<void!> {
        return this.b.flush()
    }

    func Buffered() int {
        return this.b.n
    }
}

// NewBufferedWriter buffers writes to c (size <= 0: 4096 bytes).
func NewBufferedWriter(c Conn, size int) BufferedWriter {
    var n = size
    if n <= 0 {
        n = 4096
    }
    return BufferedWriter{b: new wbuf{c: c, buf: rtBytes(n)}}
}

// ---- interfaces ----

// An address of a network interface with its prefix length.
struct IPNet {
    IP   IP
    Bits int

    func String() string {
        return std.fmt.Format("{0}/{1}", this.IP.String(), this.Bits)
    }
}

// A network interface.
struct Interface {
    Name         string
    Index        int
    Up           bool
    Loopback     bool
    Multicast    bool
    Broadcast    bool
    PointToPoint bool
    Addrs        []IPNet
}

// Interfaces lists the system's network interfaces and their addresses.
func Interfaces() []Interface! {
    var lines = try rtInterfaces() catch err {
        return opError("route", "ip+net", "", err)
    }
    var out []Interface = []
    for line in lines {
        var f = std.strings.Split(line, "\t")
        if len(f) < 5 {
            continue
        }
        var flags = f[3]
        var it = Interface{
            Name: f[0],
            Index: try std.strings.ToInt(f[1]) else 0,
            Up: std.strings.Contains(flags, "u"),
            Loopback: std.strings.Contains(flags, "l"),
            Multicast: std.strings.Contains(flags, "m"),
            Broadcast: std.strings.Contains(flags, "b"),
            PointToPoint: std.strings.Contains(flags, "p"),
        }
        if f[4] != "" {
            for a in std.strings.Split(f[4], ",") {
                var slash = std.strings.Index(a, "/")
                if slash < 0 {
                    continue
                }
                var ip = try ParseIP(a[..slash]) else IP{}
                if ip.IsValid() {
                    it.Addrs = append(it.Addrs, IPNet{
                        IP: ip,
                        Bits: try std.strings.ToInt(a[slash + 1..]) else 0,
                    })
                }
            }
        }
        out = append(out, it)
    }
    return out
}
