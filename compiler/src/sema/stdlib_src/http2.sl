namespace std.net.http

// HTTP/2 (RFC 9113) over a `conn`: frames, streams, flow control, SETTINGS,
// PING, GOAWAY (priority signals are ignored, server push is not
// implemented and disabled), header compression by std.net.http.hpack.
//
// Server: serveH2 is the connection task (it reads and dispatches frames);
// every request stream runs its handler in a task of its own with the
// ordinary Handler API (Response.Flush sends DATA frames). Client: one
// connection per host is shared by concurrent requests (the pool keeps it
// in hostPool.h2); there is no background reader task: a request that
// waits for its response reads and dispatches frames itself while it holds
// the connection's reader token, and hands the token over when it is done.

// ---- frames -----------------------------------------------------------------

// Frame types and flags (RFC 9113 6).
//   0 DATA, 1 HEADERS, 2 PRIORITY, 3 RST_STREAM, 4 SETTINGS, 5 PUSH_PROMISE,
//   6 PING, 7 GOAWAY, 8 WINDOW_UPDATE, 9 CONTINUATION
//   flags: 1 END_STREAM (ACK for SETTINGS/PING), 4 END_HEADERS, 8 PADDED, 32 PRIORITY
// Error codes: 0 NO_ERROR, 1 PROTOCOL_ERROR, 2 INTERNAL_ERROR,
//   3 FLOW_CONTROL_ERROR, 5 STREAM_CLOSED, 6 FRAME_SIZE_ERROR,
//   7 REFUSED_STREAM, 8 CANCEL, 9 COMPRESSION_ERROR, 13 HTTP_1_1_REQUIRED

struct h2frame {
    typ int
    flags int
    sid int
    payload []byte
}

// An HTTP/2 connection error (GOAWAY) or stream error (RST_STREAM).
struct H2Error {
    Code int
    Stream int
    Reason string

    func Message() string {
        var s = "http2: " + h2CodeName(this.Code)
        if this.Reason != "" {
            s += ": " + this.Reason
        }
        return s
    }
}

func h2CodeName(code int) string {
    switch code {
        case 0: return "NO_ERROR"
        case 1: return "PROTOCOL_ERROR"
        case 2: return "INTERNAL_ERROR"
        case 3: return "FLOW_CONTROL_ERROR"
        case 4: return "SETTINGS_TIMEOUT"
        case 5: return "STREAM_CLOSED"
        case 6: return "FRAME_SIZE_ERROR"
        case 7: return "REFUSED_STREAM"
        case 8: return "CANCEL"
        case 9: return "COMPRESSION_ERROR"
        case 10: return "CONNECT_ERROR"
        case 11: return "ENHANCE_YOUR_CALM"
        case 12: return "INADEQUATE_SECURITY"
        case 13: return "HTTP_1_1_REQUIRED"
    }
    return "error " + itoa(code)
}

func h2err(code int, reason string) error {
    return H2Error{Code: code, Reason: reason}
}

// Fails with err (for `raise(e)?` inside a block try).
func raise(err error) void! {
    return err
}

func put24(b []byte, n int) []byte {
    var o = b
    o = append(o, byte((n >> 16) & 255))
    o = append(o, byte((n >> 8) & 255))
    o = append(o, byte(n & 255))
    return o
}

func put32(b []byte, n int) []byte {
    var o = b
    o = append(o, byte((n >> 24) & 255))
    o = append(o, byte((n >> 16) & 255))
    o = append(o, byte((n >> 8) & 255))
    o = append(o, byte(n & 255))
    return o
}

func get32(b []byte, off int) int {
    return (int(b[off]) << 24) | (int(b[off + 1]) << 16) | (int(b[off + 2]) << 8) | int(b[off + 3])
}

// Appends one frame (header + payload[..n]) to out.
func appendFrame(out []byte, typ int, flags int, sid int, payload []byte, n int) []byte {
    var o = put24(out, n)
    o = append(o, byte(typ))
    o = append(o, byte(flags))
    o = put32(o, sid & 2147483647)
    return std.net.rtJoin(o, payload, n)
}

func frameBytes(typ int, flags int, sid int, payload []byte) []byte {
    return appendFrame([], typ, flags, sid, payload, len(payload))
}

// Reads the next frame (payload copied out of the buffer).
async func readFrame(c *conn, b *rbuf, maxSize int) h2frame! {
    await fillTo(c, b, 9)?
    var p = b.buf
    var r = b.r
    var n = (int(p[r]) << 16) | (int(p[r + 1]) << 8) | int(p[r + 2])
    var f = h2frame{typ: int(p[r + 3]), flags: int(p[r + 4]), sid: get32(p, r + 5) & 2147483647}
    if n > maxSize {
        return h2err(6, "frame of " + itoa(n) + " bytes")
    }
    b.skip(9)
    if n > 0 {
        await fillTo(c, b, n)?
        f.payload = std.bytes.Clone(b.buf[b.r..b.r + n])
        b.skip(n)
    }
    return f
}

// The payload without padding (PADDED flag) and, for HEADERS, the
// priority fields.
func unpad(f h2frame) []byte! {
    var p = f.payload
    var start = 0
    var end = len(p)
    if (f.flags & 8) != 0 {
        if len(p) < 1 {
            return h2err(1, "padded frame without a pad length")
        }
        var pad = int(p[0])
        start = 1
        if pad > end - start {
            return h2err(1, "padding longer than the frame")
        }
        end -= pad
    }
    if f.typ == 1 && (f.flags & 32) != 0 {
        if end - start < 5 {
            return h2err(6, "short HEADERS priority")
        }
        start += 5
    }
    return p[start..end]
}

// ---- events ---------------------------------------------------------------------

// A one-shot wake-up: wait() returns a task that completes on the next
// notify() (or at once if a notify came first).
struct h2event {
    guard int
    set bool
    has bool
    w Task<void>

    func wait() Task<void> {
        std.task.rtLock(&this.guard)
        if this.set {
            this.set = false
            std.task.rtUnlock(&this.guard)
            return std.task.Delay(0)
        }
        this.w = std.task.rtSignal()
        this.has = true
        std.task.rtUnlock(&this.guard)
        return this.w
    }

    func notify() {
        std.task.rtLock(&this.guard)
        if this.has {
            this.has = false
            var w = this.w
            var none Task<void>
            this.w = none
            std.task.rtUnlock(&this.guard)
            std.task.rtFire(w)
            return
        }
        this.set = true
        std.task.rtUnlock(&this.guard)
    }
}

// ---- connection and stream state ---------------------------------------------------

struct h2conn {
    c *conn
    b *rbuf
    isClient bool
    // serializes frame writes (and the HPACK encoder)
    wmu std.task.Mutex
    // guards the fields below (never held across an await)
    guard int
    enc *std.net.http.hpack.Encoder
    dec *std.net.http.hpack.Decoder
    streams map[int]*h2stream
    nextID int
    lastPeerID int
    // flow control: what we may still send on the connection
    sendWindow int
    peerInitialWindow int
    peerMaxFrame int
    peerMaxStreams int
    // what we accept: our stream window, and the connection window left
    // before we send a WINDOW_UPDATE
    ourWindow int
    recvUnacked int
    windowEv h2event
    goaway bool
    closed bool
    closeErr string
    // header block being assembled (HEADERS + CONTINUATION)
    hdrSid int
    hdrFlags int
    hdrBlock []byte
    // server
    st *serverState
    scheme string
    remote string
    active int
    wg std.task.WaitGroup
    // client: the reader token and the streams waiting for frames
    reading bool
    waiters []*h2stream
    settingsSeen bool
}

struct h2stream {
    id int
    h *h2conn
    sendWindow int
    // received
    fields []std.net.http.hpack.HeaderField
    gotHeaders bool
    body []byte
    chunks [][]byte
    ended bool
    reset bool
    resetCode int
    ev h2event
    // server: the request, and whether our HEADERS went out
    req *Request
    started bool
    finished bool
    // client: bytes consumed since the last WINDOW_UPDATE
    unacked int
    local bool
}

func newH2(c *conn, b *rbuf, client bool) *h2conn {
    var h = new h2conn{c: c, b: b, isClient: client, streams: map[int]*h2stream{}, sendWindow: 65535, peerInitialWindow: 65535, peerMaxFrame: 16384, peerMaxStreams: 100, ourWindow: 1 << 20}
    h.enc = std.net.http.hpack.NewEncoder(4096)
    h.dec = std.net.http.hpack.NewDecoder(4096)
    h.dec.MaxStringLength = 1 << 20
    if client {
        h.nextID = 1
    }
    return h
}

// Our SETTINGS (ENABLE_PUSH 0, INITIAL_WINDOW_SIZE, MAX_CONCURRENT_STREAMS,
// MAX_HEADER_LIST_SIZE) and a WINDOW_UPDATE raising the connection window.
func ourSettings(h *h2conn, maxStreams int, maxHeader int) []byte {
    var p []byte = []
    p = append(p, byte(0))
    p = append(p, byte(2))
    p = put32(p, 0)
    p = append(p, byte(0))
    p = append(p, byte(4))
    p = put32(p, h.ourWindow)
    if maxStreams > 0 {
        p = append(p, byte(0))
        p = append(p, byte(3))
        p = put32(p, maxStreams)
    }
    if maxHeader > 0 {
        p = append(p, byte(0))
        p = append(p, byte(6))
        p = put32(p, maxHeader)
    }
    var out = frameBytes(4, 0, 0, p)
    var inc = put32([], (1 << 24) - 65535)
    return std.net.rtJoin(out, frameBytes(8, 0, 0, inc), 13)
}

// Writes frames (already encoded) in one write, serialized with other writers.
async func h2write(h *h2conn, data []byte) void! {
    await h.wmu.Lock()
    var failed = ""
    try {
        _ = await h.c.write(data)?
    } catch err {
        failed = err.Message()
    }
    h.wmu.Unlock()
    if failed != "" {
        return error(failed)
    }
}

// Applies a SETTINGS frame from the peer.
func applySettings(h *h2conn, p []byte) void! {
    if len(p) % 6 != 0 {
        return h2err(6, "SETTINGS length")
    }
    var i = 0
    for i < len(p) {
        var id = (int(p[i]) << 8) | int(p[i + 1])
        var v = get32(p, i + 2)
        i += 6
        switch id {
            case 1:
                // the peer's decoder table size: our encoder may use at most this
                std.task.rtLock(&h.guard)
                h.enc.SetMaxTableSize(minInt(v, 4096))
                std.task.rtUnlock(&h.guard)
            case 2:
                if v > 1 {
                    return h2err(1, "ENABLE_PUSH " + itoa(v))
                }
            case 3:
                h.peerMaxStreams = v
            case 4:
                if v > 2147483647 {
                    return h2err(3, "INITIAL_WINDOW_SIZE too large")
                }
                std.task.rtLock(&h.guard)
                var delta = v - h.peerInitialWindow
                h.peerInitialWindow = v
                for _, s in h.streams {
                    s.sendWindow += delta
                }
                std.task.rtUnlock(&h.guard)
                h.windowEv.notify()
            case 5:
                if v < 16384 || v > 16777215 {
                    return h2err(1, "MAX_FRAME_SIZE " + itoa(v))
                }
                h.peerMaxFrame = v
        }
    }
}

// Marks the connection dead and wakes everyone waiting on it.
func h2closeAll(h *h2conn, reason string) {
    std.task.rtLock(&h.guard)
    if h.closed {
        std.task.rtUnlock(&h.guard)
        return
    }
    h.closed = true
    h.closeErr = reason
    var all []*h2stream = []
    for _, s in h.streams {
        all = append(all, s)
    }
    // the streams keep their own references; dropping them here breaks
    // the conn -> stream -> conn cycle
    h.streams = map[int]*h2stream{}
    var ws = h.waiters
    h.waiters = []
    std.task.rtUnlock(&h.guard)
    for s in all {
        s.ev.notify()
    }
    for s in ws {
        s.ev.notify()
    }
    h.windowEv.notify()
}

// HEADERS fields to a header map (canonical names; repeated fields joined;
// cookies with "; ").
func fieldsToHeaders(fs []std.net.http.hpack.HeaderField) map[string]string {
    var m = map[string]string{}
    for f in fs {
        if len(f.Name) > 0 && f.Name[0] == 58 {
            continue
        }
        var k = rtCanonical(f.Name)
        var old = m[k]
        if old == "" {
            m[k] = f.Value
        } else if k == "Cookie" {
            m[k] = old + "; " + f.Value
        } else if k == "Set-Cookie" {
            m[k] = old + "\n" + f.Value
        } else {
            m[k] = old + ", " + f.Value
        }
    }
    return m
}

func pseudo(fs []std.net.http.hpack.HeaderField, name string) string {
    for f in fs {
        if f.Name == name {
            return f.Value
        }
    }
    return ""
}

// Whether a header must not be sent over HTTP/2 (RFC 9113 8.2.2).
func connectionSpecific(k string) bool {
    return k == "Connection" || k == "Keep-Alive" || k == "Proxy-Connection" || k == "Transfer-Encoding" || k == "Upgrade" || k == "Host"
}

// Appends the regular fields of a header map (lower-case names).
func appendFields(out []std.net.http.hpack.HeaderField, h map[string]string) []std.net.http.hpack.HeaderField {
    var fs = out
    for k, v in h {
        var ck = rtCanonical(k)
        if connectionSpecific(ck) || ck == "Content-Length" {
            continue
        }
        var lk = std.strings.ToLower(ck)
        var sensitive = lk == "authorization" || lk == "proxy-authorization" || (lk == "cookie" && len(v) < 20)
        if lk == "set-cookie" {
            for part in std.strings.Split(v, "\n") {
                fs = append(fs, std.net.http.hpack.HeaderField{Name: lk, Value: part})
            }
        } else {
            fs = append(fs, std.net.http.hpack.HeaderField{Name: lk, Value: v, Sensitive: sensitive})
        }
    }
    return fs
}

// Encodes fields and writes HEADERS (+ CONTINUATION) for sid, plus `tail`
// (more frames, e.g. a small DATA) in the same write.
async func h2writeHeaders(h *h2conn, sid int, fs []std.net.http.hpack.HeaderField, endStream bool, tail []byte) void! {
    await h.wmu.Lock()
    var block = h.enc.Encode(fs)
    var out []byte = []
    var max = h.peerMaxFrame
    var off = 0
    var first = true
    for first || off < len(block) {
        var n = minInt(max, len(block) - off)
        var flags = 0
        if off + n == len(block) {
            flags = 4
        }
        var typ = 9
        if first {
            typ = 1
            if endStream {
                flags = flags | 1
            }
        }
        out = appendFrame(out, typ, flags, sid, block[off..], n)
        off += n
        first = false
    }
    if len(tail) > 0 {
        out = std.net.rtJoin(out, tail, len(tail))
    }
    var failed = ""
    try {
        _ = await h.c.write(out)?
    } catch err {
        failed = err.Message()
    }
    h.wmu.Unlock()
    if failed != "" {
        return error(failed)
    }
}

// Sends data on stream s as DATA frames within the flow-control windows;
// END_STREAM on the last one when end.
async func h2sendData(h *h2conn, s *h2stream, data []byte, end bool) void! {
    var off = 0
    if len(data) == 0 {
        if end {
            await h2write(h, frameBytes(0, 1, s.id, []))?
        }
        return
    }
    for off < len(data) {
        std.task.rtLock(&h.guard)
        if h.closed {
            std.task.rtUnlock(&h.guard)
            return h2err(8, "connection closed")
        }
        if s.reset {
            std.task.rtUnlock(&h.guard)
            return h2err(s.resetCode, "stream reset by the peer")
        }
        var avail = minInt(minInt(h.sendWindow, s.sendWindow), h.peerMaxFrame)
        if avail <= 0 {
            var w = h.windowEv.wait()
            std.task.rtUnlock(&h.guard)
            await w
            if std.task.IsCancelled() {
                return std.task.ErrCancelled
            }
            continue
        }
        var k = minInt(avail, len(data) - off)
        h.sendWindow -= k
        s.sendWindow -= k
        std.task.rtUnlock(&h.guard)
        var flags = 0
        if end && off + k == len(data) {
            flags = 1
        }
        await h2write(h, appendFrame([], 0, flags, s.id, data[off..], k))?
        off += k
    }
}

func h2rst(sid int, code int) []byte {
    return frameBytes(3, 0, sid, put32([], code))
}

func h2goaway(last int, code int, reason string) []byte {
    var p = put32([], last)
    p = put32(p, code)
    return frameBytes(7, 0, 0, std.net.rtJoin(p, std.strings.Bytes(reason), len(reason)))
}

// Sends WINDOW_UPDATEs for n consumed bytes of stream sid (and the connection).
async func h2ack(h *h2conn, sid int, n int) void! {
    if n <= 0 {
        return
    }
    var out = frameBytes(8, 0, 0, put32([], n))
    if sid > 0 {
        out = std.net.rtJoin(out, frameBytes(8, 0, sid, put32([], n)), 13)
    }
    await h2write(h, out)?
}

// Handles a frame common to both sides (SETTINGS, PING, WINDOW_UPDATE,
// RST_STREAM, GOAWAY, PRIORITY); true if it was one of those.
async func h2control(h *h2conn, f h2frame) bool! {
    switch f.typ {
        case 4:
            if f.sid != 0 {
                return h2err(1, "SETTINGS on a stream")
            }
            if (f.flags & 1) != 0 {
                if len(f.payload) != 0 {
                    return h2err(6, "SETTINGS ACK with a payload")
                }
                return true
            }
            applySettings(h, f.payload)?
            h.settingsSeen = true
            await h2write(h, frameBytes(4, 1, 0, []))?
            return true
        case 6:
            if len(f.payload) != 8 || f.sid != 0 {
                return h2err(6, "PING")
            }
            if (f.flags & 1) == 0 {
                await h2write(h, frameBytes(6, 1, 0, f.payload))?
            }
            return true
        case 8:
            if len(f.payload) != 4 {
                return h2err(6, "WINDOW_UPDATE length")
            }
            var inc = get32(f.payload, 0) & 2147483647
            if inc == 0 {
                if f.sid == 0 {
                    return h2err(1, "WINDOW_UPDATE of 0")
                }
                await h2write(h, h2rst(f.sid, 1))?
                return true
            }
            std.task.rtLock(&h.guard)
            if f.sid == 0 {
                h.sendWindow += inc
                if h.sendWindow > 2147483647 {
                    std.task.rtUnlock(&h.guard)
                    return h2err(3, "connection window overflow")
                }
            } else {
                var s = h.streams[f.sid]
                if s != nil {
                    s.sendWindow += inc
                    if s.sendWindow > 2147483647 {
                        s.reset = true
                        s.resetCode = 3
                    }
                }
            }
            std.task.rtUnlock(&h.guard)
            h.windowEv.notify()
            return true
        case 3:
            if len(f.payload) != 4 {
                return h2err(6, "RST_STREAM length")
            }
            if f.sid == 0 {
                return h2err(1, "RST_STREAM on stream 0")
            }
            std.task.rtLock(&h.guard)
            var s = h.streams[f.sid]
            if s != nil {
                s.reset = true
                s.resetCode = get32(f.payload, 0)
            }
            std.task.rtUnlock(&h.guard)
            if s != nil {
                s.ev.notify()
                h.windowEv.notify()
            }
            return true
        case 7:
            if len(f.payload) < 8 {
                return h2err(6, "GOAWAY length")
            }
            var last = get32(f.payload, 0) & 2147483647
            std.task.rtLock(&h.guard)
            h.goaway = true
            var refused []*h2stream = []
            if h.isClient {
                for id, s in h.streams {
                    if id > last {
                        s.reset = true
                        s.resetCode = 7
                        refused = append(refused, s)
                    }
                }
            }
            std.task.rtUnlock(&h.guard)
            for s in refused {
                s.ev.notify()
            }
            return true
        case 2:
            if len(f.payload) != 5 {
                return h2err(6, "PRIORITY length")
            }
            return true
        case 5:
            return h2err(1, "PUSH_PROMISE (push is disabled)")
    }
    return false
}

// HEADERS/CONTINUATION assembly: the complete block, or nil while
// CONTINUATION frames are expected.
func h2headerBlock(h *h2conn, f h2frame) []byte! {
    if f.typ == 1 {
        if len(h.hdrBlock) > 0 || h.hdrSid != 0 {
            return h2err(1, "HEADERS while a header block is open")
        }
        var frag = unpad(f)?
        if (f.flags & 4) != 0 {
            return std.bytes.Clone(frag)
        }
        h.hdrSid = f.sid
        h.hdrFlags = f.flags
        h.hdrBlock = std.bytes.Clone(frag)
        return nil
    }
    if h.hdrSid == 0 || f.sid != h.hdrSid {
        return h2err(1, "unexpected CONTINUATION")
    }
    h.hdrBlock = std.net.rtJoin(h.hdrBlock, f.payload, len(f.payload))
    if len(h.hdrBlock) > (1 << 20) {
        return h2err(11, "header block too large")
    }
    if (f.flags & 4) == 0 {
        return nil
    }
    var blk = h.hdrBlock
    h.hdrBlock = []
    h.hdrSid = 0
    return blk
}

// ---- server ---------------------------------------------------------------------

// Serves one HTTP/2 connection; b holds bytes read so far (the h2c
// preface may already be there).
async func serveH2(st *serverState, e *connEntry, b *rbuf) {
    var c = e.c
    var h = newH2(c, b, false)
    h.st = st
    h.scheme = "http"
    if c.isTLS {
        h.scheme = "https"
    }
    h.remote = c.remoteAddr()
    e.h2 = h
    e.idle = false
    c.setTimeout(st.idle)
    var reason = ""
    var code = 0
    try {
        await fillTo(c, b, 24)?
        var pre = std.strings.FromBytes(b.buf[b.r..b.r + 24])
        if pre != "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" {
            return
        }
        b.skip(24)
        await h2write(h, ourSettings(h, st.maxStreams, st.maxHead))?
        var first = true
        for {
            std.task.rtLock(&h.guard)
            var idle = h.active == 0
            var done = h.goaway && h.active == 0
            std.task.rtUnlock(&h.guard)
            if done {
                break
            }
            if idle {
                c.setTimeout(st.idle)
            } else {
                c.setTimeout(0)
            }
            var f = await readFrame(c, b, 16384)?
            if first && f.typ != 4 {
                raise(h2err(1, "the client preface must start with SETTINGS"))?
            }
            first = false
            if h.hdrSid != 0 && f.typ != 9 {
                raise(h2err(1, "frame inside a header block"))?
            }
            if await h2control(h, f)? {
                continue
            }
            if f.typ == 1 || f.typ == 9 {
                var blk = h2headerBlock(h, f)?
                if blk == nil {
                    continue
                }
                var flags = f.flags
                var sid = f.sid
                if f.typ == 9 {
                    flags = h.hdrFlags
                }
                await h2serverHeaders(h, sid, flags, blk)?
            } else if f.typ == 0 {
                await h2serverData(h, f)?
            }
        }
    } catch err {
        switch err {
            case H2Error he:
                code = he.Code
                reason = he.Reason
            default:
                reason = err.Message()
        }
    }
    if code != 0 {
        try await h2write(h, h2goaway(h.lastPeerID, code, reason)) catch err {}
    }
    h2closeAll(h, reason)
    // wait for the handlers still running (their writes fail fast now)
    await h.wg.Wait()
    std.task.rtLock(&h.guard)
    h.streams = map[int]*h2stream{}
    std.task.rtUnlock(&h.guard)
}

async func h2serverHeaders(h *h2conn, sid int, flags int, blk []byte) void! {
    var fs = try h.dec.Decode(blk) catch err {
        return h2err(9, err.Message())
    }
    std.task.rtLock(&h.guard)
    var s = h.streams[sid]
    std.task.rtUnlock(&h.guard)
    if s != nil {
        // trailers: must end the stream
        if (flags & 1) == 0 {
            return h2err(1, "trailers without END_STREAM")
        }
        s.ended = true
        h2startStream(h, s)
        return
    }
    if sid % 2 == 0 || sid <= h.lastPeerID {
        return h2err(1, "invalid stream id " + itoa(sid))
    }
    h.lastPeerID = sid
    std.task.rtLock(&h.guard)
    var refuse = h.active >= h.st.maxStreams || h.goaway
    std.task.rtUnlock(&h.guard)
    if refuse {
        await h2write(h, h2rst(sid, 7))?
        return
    }
    var method = pseudo(fs, ":method")
    var path = pseudo(fs, ":path")
    if method == "" || (path == "" && method != "CONNECT") {
        await h2write(h, h2rst(sid, 1))?
        return
    }
    var req = new Request{Method: method, Proto: "HTTP/2.0", Headers: fieldsToHeaders(fs), RemoteAddr: h.remote, Scheme: h.scheme}
    req.Host = pseudo(fs, ":authority")
    if req.Host == "" {
        req.Host = req.Headers["Host"]
    }
    var q = std.strings.Index(path, "?")
    if q >= 0 {
        req.Query = path[q + 1..]
        path = path[..q]
    }
    req.Path = path
    s = new h2stream{id: sid, h: h, req: req}
    std.task.rtLock(&h.guard)
    s.sendWindow = h.peerInitialWindow
    h.streams[sid] = s
    h.active += 1
    std.task.rtUnlock(&h.guard)
    h.wg.Add(1)
    if (flags & 1) != 0 {
        s.ended = true
        h2startStream(h, s)
    }
}

async func h2serverData(h *h2conn, f h2frame) void! {
    std.task.rtLock(&h.guard)
    var s = h.streams[f.sid]
    std.task.rtUnlock(&h.guard)
    var data = unpad(f)?
    // flow control counts the whole frame, padding included
    h.recvUnacked += len(f.payload)
    if s == nil || s.ended {
        if f.sid == 0 || f.sid > h.lastPeerID {
            return h2err(1, "DATA on an idle stream")
        }
        await h2ack(h, 0, len(f.payload))?
        h.recvUnacked -= len(f.payload)
        if s != nil {
            await h2write(h, h2rst(f.sid, 5))?
        }
        return
    }
    if h.st.maxBody > 0 && len(s.body) + len(data) > h.st.maxBody {
        s.ended = true
        s.reset = true
        await h2write(h, h2rst(f.sid, 8))?
        h2startStream(h, s)
        return
    }
    s.body = std.net.rtJoin(s.body, data, len(data))
    s.unacked += len(f.payload)
    var ended = (f.flags & 1) != 0
    // the body is buffered as it comes: give the windows back in 32 KB steps
    // (a stream that has ended needs no more credit)
    var out []byte = []
    if h.recvUnacked >= (1 << 15) {
        out = frameBytes(8, 0, 0, put32([], h.recvUnacked))
        h.recvUnacked = 0
    }
    if !ended && s.unacked >= (1 << 15) {
        out = std.net.rtJoin(out, frameBytes(8, 0, f.sid, put32([], s.unacked)), 13)
        s.unacked = 0
    }
    if len(out) > 0 {
        await h2write(h, out)?
    }
    if ended {
        s.ended = true
        h2startStream(h, s)
    }
}

// Starts the handler task of a complete request stream.
func h2startStream(h *h2conn, s *h2stream) {
    if s.started || s.finished {
        return
    }
    s.started = true
    _ = h2runStream(h, s)
}

async func h2runStream(h *h2conn, s *h2stream) {
    await std.task.Yield()
    var st = h.st
    var req = s.req
    req.Body = std.strings.FromBytes(s.body)
    s.body = []
    var w = new respWriter{h2: s, proto: "HTTP/2.0", keep: true, headOnly: req.Method == "HEAD", noDate: st.noDate}
    var res = new Response{Status: 200, w: w}
    s.started = false
    if s.reset && s.resetCode == 8 {
        res = errorResponse(413, ErrBodyTooLarge.Message())
        res.w = w
    } else {
        var trace = req.Headers["Traceparent"]
        var remoteSpan = std.diag.enterRemote(trace)
        var failure = ""
        var aborted = false
        try await st.handler.Serve(req, res) catch err {
            failure = err.Message()
            if failure == "" {
                failure = "handler failed"
            }
            if std.errors.Is(err, ErrAbortHandler) {
                aborted = true
            }
        }
        std.diag.leaveRemote(remoteSpan)
        if failure != "" {
            if aborted || w.started {
                try await h2write(h, h2rst(s.id, 2)) catch err {}
                h2endStream(h, s)
                return
            }
            res = errorResponse(500, failure)
            res.w = w
        }
    }
    try await h2Flush(s, res, true) catch err {}
    h2endStream(h, s)
}

func h2endStream(h *h2conn, s *h2stream) {
    std.task.rtLock(&h.guard)
    s.finished = true
    delete(h.streams, s.id)
    h.active -= 1
    var last = h.active == 0 && h.goaway
    std.task.rtUnlock(&h.guard)
    s.req = nil
    h.wg.Done()
    if last {
        // GOAWAY sent and nothing left: end the connection
        h.c.close()
    }
}

// The response HEADERS fields.
func h2responseFields(w *respWriter, res *Response, status int, length int) []std.net.http.hpack.HeaderField {
    var fs []std.net.http.hpack.HeaderField = [std.net.http.hpack.HeaderField{Name: ":status", Value: itoa(status)}]
    fs = appendFields(fs, res.Headers)
    var hasType = res.Headers != nil && res.Headers["Content-Type"] != ""
    if !hasType && !bodyless(status) && (length != 0) {
        fs = append(fs, std.net.http.hpack.HeaderField{Name: "content-type", Value: "text/plain; charset=utf-8"})
    }
    if !w.noDate && (res.Headers == nil || res.Headers["Date"] == "") {
        fs = append(fs, std.net.http.hpack.HeaderField{Name: "date", Value: rtDate()})
    }
    if length >= 0 && !bodyless(status) {
        fs = append(fs, std.net.http.hpack.HeaderField{Name: "content-length", Value: itoa(length)})
    }
    return fs
}

// Response.Flush / the end of a handler on HTTP/2: HEADERS once, then the
// body written so far as DATA (END_STREAM when end).
async func h2Flush(s *h2stream, res *Response, end bool) void! {
    var h = s.h
    var w = res.w
    var status = res.Status
    if status == 0 {
        status = 200
    }
    var body = res.Body
    res.Body = ""
    if res.file != "" && end {
        var f = std.os.Open(res.file)?
        var buf = std.net.MakeBuffer(res.fileLen)
        var k = try f.ReadAt(buf, res.fileOff) catch err {
            _ = f.Close()
            return err
        }
        _ = f.Close()
        body = std.strings.FromBytes(buf[..k])
        res.file = ""
    }
    var length = -1
    if end {
        length = len(body)
        if w.headOnly && res.fileLen > 0 {
            length = res.fileLen
        }
    }
    if w.headOnly || bodyless(status) {
        body = ""
    }
    if !w.started {
        w.started = true
        var fs = h2responseFields(w, res, status, length)
        if len(body) == 0 {
            await h2writeHeaders(h, s.id, fs, end, [])?
            return
        }
        var data = std.strings.Bytes(body)
        std.task.rtLock(&h.guard)
        var fits = len(data) <= minInt(minInt(h.sendWindow, s.sendWindow), h.peerMaxFrame)
        if fits {
            h.sendWindow -= len(data)
            s.sendWindow -= len(data)
        }
        std.task.rtUnlock(&h.guard)
        if fits {
            // HEADERS and the whole body in one write
            var flags = 0
            if end {
                flags = 1
            }
            await h2writeHeaders(h, s.id, fs, false, frameBytes(0, flags, s.id, data))?
            return
        }
        await h2writeHeaders(h, s.id, fs, false, [])?
        await h2sendData(h, s, data, end)?
        return
    }
    await h2sendData(h, s, std.strings.Bytes(body), end)?
}

// Server.Shutdown on an HTTP/2 connection: GOAWAY, finish the streams in
// flight, then close.
func h2Shutdown(h *h2conn) {
    _ = h2sendShutdown(h)
}

async func h2sendShutdown(h *h2conn) {
    std.task.rtLock(&h.guard)
    var already = h.goaway
    h.goaway = true
    var idle = h.active == 0
    std.task.rtUnlock(&h.guard)
    if !already {
        try await h2write(h, h2goaway(h.lastPeerID, 0, "")) catch err {}
    }
    if idle {
        h.c.close()
    }
}

// ---- client ---------------------------------------------------------------------

// Starts an HTTP/2 client connection: the preface and our SETTINGS.
async func startH2Client(c *conn, b *rbuf) *h2conn! {
    var h = newH2(c, b, true)
    var pre = std.strings.Bytes("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")
    await h2write(h, std.net.rtJoin(pre, ourSettings(h, 0, 0), 1 << 30))?
    return h
}

func h2Usable(h *h2conn) bool {
    std.task.rtLock(&h.guard)
    var ok = !h.closed && !h.goaway && len(h.streams) < h.peerMaxStreams && h.nextID < 2147483000
    std.task.rtUnlock(&h.guard)
    return ok
}

func h2Retryable(err error) bool {
    switch err {
        case H2Error he:
            return he.Code == 7 || (he.Code == 0 && he.Reason == "connection closed")
    }
    return false
}

// Waits until ready() holds for s, reading frames while holding the
// connection's reader token.
async func h2await(h *h2conn, s *h2stream, ready func() bool) void! {
    for {
        if ready() {
            return
        }
        std.task.rtLock(&h.guard)
        if h.closed {
            var why = h.closeErr
            std.task.rtUnlock(&h.guard)
            if ready() {
                return
            }
            if why == "" {
                why = "connection closed"
            }
            return H2Error{Code: 0, Reason: why}
        }
        if h.reading {
            h.waiters = append(h.waiters, s)
            var w = s.ev.wait()
            std.task.rtUnlock(&h.guard)
            await w
            if std.task.IsCancelled() {
                return std.task.ErrCancelled
            }
            continue
        }
        h.reading = true
        std.task.rtUnlock(&h.guard)
        var failed = ""
        try {
            for !ready() {
                var f = await readFrame(h.c, h.b, 16384)?
                await h2clientFrame(h, f)?
            }
        } catch err {
            failed = err.Message()
            switch err {
                case H2Error he:
                    if he.Code != 0 && he.Stream == 0 {
                        try await h2write(h, h2goaway(0, he.Code, he.Reason)) catch err {}
                    }
            }
        }
        // hand the token over: everyone waiting re-checks
        std.task.rtLock(&h.guard)
        h.reading = false
        var ws = h.waiters
        h.waiters = []
        std.task.rtUnlock(&h.guard)
        for x in ws {
            x.ev.notify()
        }
        if failed != "" {
            h2closeAll(h, failed)
            h.c.close()
            if !ready() {
                return error(failed)
            }
        }
    }
}

async func h2clientFrame(h *h2conn, f h2frame) void! {
    if h.hdrSid != 0 && f.typ != 9 {
        return h2err(1, "frame inside a header block")
    }
    if await h2control(h, f)? {
        if f.typ == 7 {
            std.task.rtLock(&h.guard)
            var none = len(h.streams) == 0
            std.task.rtUnlock(&h.guard)
            if none {
                h2closeAll(h, "GOAWAY")
            }
        }
        return
    }
    if f.typ == 1 || f.typ == 9 {
        var blk = h2headerBlock(h, f)?
        if blk == nil {
            return
        }
        var flags = f.flags
        if f.typ == 9 {
            flags = h.hdrFlags
        }
        var fs = try h.dec.Decode(blk) catch err {
            return h2err(9, err.Message())
        }
        std.task.rtLock(&h.guard)
        var s = h.streams[f.sid]
        std.task.rtUnlock(&h.guard)
        if s == nil {
            return
        }
        var status = rtAtoi(pseudo(fs, ":status"))
        if !s.gotHeaders && status >= 100 && status < 200 {
            return
        }
        std.task.rtLock(&h.guard)
        if !s.gotHeaders {
            s.fields = fs
            s.gotHeaders = true
        }
        if (flags & 1) != 0 {
            s.ended = true
        }
        std.task.rtUnlock(&h.guard)
        s.ev.notify()
        return
    }
    if f.typ == 0 {
        var data = unpad(f)?
        std.task.rtLock(&h.guard)
        var s = h.streams[f.sid]
        std.task.rtUnlock(&h.guard)
        if s == nil {
            // a stream we gave up: keep the connection window open
            await h2ack(h, 0, len(f.payload))?
            return
        }
        var piece = std.bytes.Clone(data)
        std.task.rtLock(&h.guard)
        if len(piece) > 0 {
            s.chunks = append(s.chunks, piece)
        }
        if (f.flags & 1) != 0 {
            s.ended = true
        }
        std.task.rtUnlock(&h.guard)
        var pad = len(f.payload) - len(data)
        if pad > 0 {
            await h2ack(h, 0, pad)?
        }
        s.ev.notify()
    }
}

// One request on a shared HTTP/2 connection; the body is left to a
// BodyReader (res.Reader).
async func h2RoundTrip(h *h2conn, method string, u URL, hd map[string]string, body string, hasBody bool, gz bool) Response! {
    std.task.rtLock(&h.guard)
    if h.closed || h.goaway {
        std.task.rtUnlock(&h.guard)
        return H2Error{Code: 0, Reason: "connection closed"}
    }
    var s = new h2stream{id: h.nextID, h: h, sendWindow: h.peerInitialWindow, local: true}
    h.nextID += 2
    h.streams[s.id] = s
    std.task.rtUnlock(&h.guard)
    var fs []std.net.http.hpack.HeaderField = []
    fs = append(fs, std.net.http.hpack.HeaderField{Name: ":method", Value: method})
    fs = append(fs, std.net.http.hpack.HeaderField{Name: ":scheme", Value: u.Scheme})
    fs = append(fs, std.net.http.hpack.HeaderField{Name: ":authority", Value: u.Host})
    fs = append(fs, std.net.http.hpack.HeaderField{Name: ":path", Value: u.RequestURI()})
    fs = appendFields(fs, hd)
    if hd["User-Agent"] == "" {
        fs = append(fs, std.net.http.hpack.HeaderField{Name: "user-agent", Value: "slang-http/0.1"})
    }
    if hd["Accept"] == "" {
        fs = append(fs, std.net.http.hpack.HeaderField{Name: "accept", Value: "*/*"})
    }
    if gz {
        fs = append(fs, std.net.http.hpack.HeaderField{Name: "accept-encoding", Value: "gzip"})
    }
    var traceparent = std.diag.Traceparent()
    if traceparent != "" && hd["Traceparent"] == "" {
        fs = append(fs, std.net.http.hpack.HeaderField{Name: "traceparent", Value: traceparent})
    }
    var withBody = hasBody || body != "" || method == "POST" || method == "PUT" || method == "PATCH"
    if withBody {
        fs = append(fs, std.net.http.hpack.HeaderField{Name: "content-length", Value: itoa(len(body))})
    }
    try {
        await h2writeHeaders(h, s.id, fs, !withBody, [])?
        if withBody {
            await h2sendData(h, s, std.strings.Bytes(body), true)?
        }
        await h2await(h, s, func() bool {
            std.task.rtLock(&h.guard)
            var ok = s.gotHeaders || s.reset
            std.task.rtUnlock(&h.guard)
            return ok
        })?
    } catch err {
        h2forget(h, s)
        return err
    }
    if s.reset && !s.gotHeaders {
        h2forget(h, s)
        return H2Error{Code: s.resetCode, Stream: s.id, Reason: "stream reset"}
    }
    var res = Response{Status: rtAtoi(pseudo(s.fields, ":status")), Headers: fieldsToHeaders(s.fields), Proto: "HTTP/2.0"}
    s.fields = []
    var br = new BodyReader{h2: s, mode: 0}
    if gz && res.Headers["Content-Encoding"] == "gzip" {
        br.gz = std.compress.gzip.NewReader()
        delete(res.Headers, "Content-Encoding")
        delete(res.Headers, "Content-Length")
    }
    res.Reader = br
    return res
}

// The next piece of a response body; empty at END_STREAM.
async func h2ReadBody(s *h2stream, n int) []byte! {
    var h = s.h
    if h == nil {
        return []
    }
    await h2await(h, s, func() bool {
        std.task.rtLock(&h.guard)
        var ok = len(s.chunks) > 0 || s.ended || s.reset
        std.task.rtUnlock(&h.guard)
        return ok
    })?
    std.task.rtLock(&h.guard)
    if len(s.chunks) == 0 {
        var wasReset = s.reset && !s.ended
        std.task.rtUnlock(&h.guard)
        h2forget(h, s)
        if wasReset {
            return H2Error{Code: s.resetCode, Stream: s.id, Reason: "stream reset"}
        }
        return []
    }
    var c = s.chunks[0]
    var out = c
    if len(c) > n {
        out = std.bytes.Clone(c[..n])
        s.chunks[0] = std.bytes.Clone(c[n..])
    } else {
        var none []byte
        s.chunks[0] = none
        s.chunks = s.chunks[1..]
    }
    var drained = s.ended && len(s.chunks) == 0
    std.task.rtUnlock(&h.guard)
    s.unacked += len(out)
    if s.unacked >= 16384 || drained {
        var k = s.unacked
        s.unacked = 0
        if !s.ended {
            try await h2ack(h, s.id, k) catch err {}
        } else {
            try await h2ack(h, 0, k) catch err {}
        }
    }
    return out
}

// Drops a client stream from the connection.
func h2forget(h *h2conn, s *h2stream) {
    std.task.rtLock(&h.guard)
    delete(h.streams, s.id)
    var gone = h.goaway && len(h.streams) == 0
    std.task.rtUnlock(&h.guard)
    s.h = nil
    if gone {
        h2closeAll(h, "GOAWAY")
        h.c.close()
    }
}

// BodyReader.Close on HTTP/2: RST_STREAM(CANCEL) unless finished.
func h2CancelStream(s *h2stream) {
    var h = s.h
    if h == nil {
        return
    }
    if !s.ended {
        _ = h2write(h, h2rst(s.id, 8))
    }
    h2forget(h, s)
}
