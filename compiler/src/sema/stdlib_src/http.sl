namespace std.net.http

// The Slang-source std.net.http v1 (see sema/stdsrc.rs for the rules):
// HTTP/1.1 over plain TCP (std.net). Requests and responses are parsed
// here, in Slang, on top of a small buffered reader. Bodies are strings
// (byte strings: any bytes are allowed). https URLs and Server.TLS use
// std.net.tls; both sides talk to a `stream` (std.net.Conn or tls.Conn).

// ---- shared helpers ---------------------------------------------------

// The canonical form of a header name: "content-type" -> "Content-Type".
func CanonicalHeader(name string) string {
    var parts = std.strings.Split(std.strings.ToLower(std.strings.Trim(name)), "-")
    var out = ""
    for i, p in parts {
        if i > 0 {
            out += "-"
        }
        if len(p) > 0 {
            out += std.strings.ToUpper(p[..1]) + p[1..]
        }
    }
    return out
}

// The reason phrase of a status code ("Not Found"); "" if unknown.
func StatusText(code int) string {
    switch code {
        case 100: return "Continue"
        case 101: return "Switching Protocols"
        case 200: return "OK"
        case 201: return "Created"
        case 202: return "Accepted"
        case 204: return "No Content"
        case 206: return "Partial Content"
        case 301: return "Moved Permanently"
        case 302: return "Found"
        case 303: return "See Other"
        case 304: return "Not Modified"
        case 307: return "Temporary Redirect"
        case 308: return "Permanent Redirect"
        case 400: return "Bad Request"
        case 401: return "Unauthorized"
        case 403: return "Forbidden"
        case 404: return "Not Found"
        case 405: return "Method Not Allowed"
        case 408: return "Request Timeout"
        case 409: return "Conflict"
        case 411: return "Length Required"
        case 413: return "Content Too Large"
        case 415: return "Unsupported Media Type"
        case 422: return "Unprocessable Content"
        case 429: return "Too Many Requests"
        case 431: return "Request Header Fields Too Large"
        case 500: return "Internal Server Error"
        case 501: return "Not Implemented"
        case 502: return "Bad Gateway"
        case 503: return "Service Unavailable"
        case 504: return "Gateway Timeout"
        case 505: return "HTTP Version Not Supported"
    }
    return ""
}

func itoa(n int) string {
    return std.fmt.Format("{0}", n)
}

func parseHex(s string) int! {
    var t = std.strings.Trim(s)
    if len(t) == 0 {
        return error("invalid chunk size \"" + s + "\"")
    }
    var n = 0
    for i in 0..len(t) {
        var c = int(t[i])
        var d = -1
        if c >= 48 && c <= 57 {
            d = c - 48
        } else if c >= 97 && c <= 102 {
            d = c - 87
        } else if c >= 65 && c <= 70 {
            d = c - 55
        }
        if d < 0 || n > 576460752303423487 {
            return error("invalid chunk size \"" + s + "\"")
        }
        n = n * 16 + d
    }
    return n
}

// Percent-decoding of a query component ('+' is a space).
func unescape(s string) string {
    if !std.strings.Contains(s, "%") && !std.strings.Contains(s, "+") {
        return s
    }
    var out []byte = []
    var i = 0
    for i < len(s) {
        var c = s[i]
        if c == 43 {
            out = append(out, byte(32))
            i += 1
        } else if c == 37 && i + 2 < len(s) {
            var v = try parseHex(s[i + 1..i + 3]) else -1
            if v < 0 {
                out = append(out, c)
                i += 1
            } else {
                out = append(out, byte(v))
                i += 3
            }
        } else {
            out = append(out, c)
            i += 1
        }
    }
    return std.strings.FromBytes(out)
}

// Parses "a=1&b=x%20y" into a map (the last value of a repeated key wins).
func ParseQuery(q string) map[string]string {
    var m = map[string]string{}
    if q == "" {
        return m
    }
    for kv in std.strings.Split(q, "&") {
        if kv == "" {
            continue
        }
        var i = std.strings.Index(kv, "=")
        if i < 0 {
            m[unescape(kv)] = ""
        } else {
            m[unescape(kv[..i])] = unescape(kv[i + 1..])
        }
    }
    return m
}

// A byte stream: a std.net.Conn or a std.net.tls.Conn.
interface stream {
    func Read(n int) Task<[]byte!>
    func WriteString(text string) Task<int!>
    func SetTimeout(ms int)
    func Close()
    func IsClosed() bool
    func RemoteAddr() string
}

// A buffered reader over a connection.
struct reader {
    c stream
    buf string
    eof bool

    async func fill() void! {
        var b = await this.c.Read(16384)?
        if len(b) == 0 {
            this.eof = true
        } else {
            this.buf += std.strings.FromBytes(b)
        }
    }

    // Reads up to and including the blank line that ends a header block;
    // "" if the stream ended before any byte.
    async func readHead(limit int) string! {
        for {
            var i = std.strings.Index(this.buf, "\r\n\r\n")
            if i >= 0 {
                var head = this.buf[..i + 4]
                this.buf = this.buf[i + 4..]
                return head
            }
            if len(this.buf) > limit {
                return error("header too large")
            }
            if this.eof {
                if len(this.buf) == 0 {
                    return ""
                }
                return error("unexpected end of stream in header")
            }
            await this.fill()?
        }
    }

    async func readLine() string! {
        for {
            var i = std.strings.Index(this.buf, "\r\n")
            if i >= 0 {
                var line = this.buf[..i]
                this.buf = this.buf[i + 2..]
                return line
            }
            if this.eof {
                return error("unexpected end of stream")
            }
            await this.fill()?
        }
    }

    async func readN(n int) string! {
        for len(this.buf) < n {
            if this.eof {
                return error("unexpected end of stream in body")
            }
            await this.fill()?
        }
        var s = this.buf[..n]
        this.buf = this.buf[n..]
        return s
    }

    async func readAll() string! {
        for !this.eof {
            await this.fill()?
        }
        var s = this.buf
        this.buf = ""
        return s
    }

    // A chunked body (RFC 9112 section 7.1); trailers are skipped.
    async func readChunked() string! {
        var body = ""
        for {
            var line = await this.readLine()?
            var semi = std.strings.Index(line, ";")
            if semi >= 0 {
                line = line[..semi]
            }
            var n = parseHex(line)?
            if n == 0 {
                for {
                    var t = await this.readLine()?
                    if t == "" {
                        return body
                    }
                }
            }
            body += await this.readN(n)?
            var crlf = await this.readLine()?
            if crlf != "" {
                return error("malformed chunk")
            }
        }
    }
}

// Parses header lines (after the first line) of a header block.
func parseHeaders(lines []string) map[string]string! {
    var h = map[string]string{}
    for i in 1..len(lines) {
        var line = lines[i]
        if line == "" {
            continue
        }
        var c = std.strings.Index(line, ":")
        if c <= 0 {
            return error("malformed header line \"" + line + "\"")
        }
        var k = CanonicalHeader(line[..c])
        var v = std.strings.Trim(line[c + 1..])
        var old = h[k]
        if old != "" {
            h[k] = old + ", " + v
        } else {
            h[k] = v
        }
    }
    return h
}

func headerHas(h map[string]string, name string, token string) bool {
    for part in std.strings.Split(h[name], ",") {
        if std.strings.EqualFold(std.strings.Trim(part), token) {
            return true
        }
    }
    return false
}

// ---- messages ------------------------------------------------------------

// Request is an incoming request on the server.
struct Request {
    Method string
    // The path without the query ("/users/7").
    Path string
    // The raw query after '?' ("" if none).
    Query string
    Proto string
    Headers map[string]string
    Body string
    // Path parameters set by the Router ({id} -> Params["id"]).
    Params map[string]string
    RemoteAddr string

    func Header(name string) string {
        return this.Headers[CanonicalHeader(name)]
    }

    func Param(name string) string {
        return this.Params[name]
    }

    // A query parameter (decoded), "" if absent.
    func QueryValue(name string) string {
        var m = ParseQuery(this.Query)
        return m[name]
    }
}

// Response is the response of a client request, and what a server handler
// fills in (Status defaults to 200, Body is sent with a Content-Length).
struct Response {
    Status int
    Headers map[string]string
    Body string

    func Header(name string) string {
        return this.Headers[CanonicalHeader(name)]
    }

    func SetHeader(name string, value string) {
        if this.Headers == nil {
            this.Headers = map[string]string{}
        }
        this.Headers[CanonicalHeader(name)] = value
    }

    // Appends to the body.
    func Write(text string) {
        this.Body += text
    }

    // Sets status, Content-Type text/plain and body.
    func Text(status int, body string) {
        this.Status = status
        this.SetHeader("Content-Type", "text/plain; charset=utf-8")
        this.Body = body
    }

    // Sets status, Content-Type application/json and body (already JSON).
    func Json(status int, body string) {
        this.Status = status
        this.SetHeader("Content-Type", "application/json")
        this.Body = body
    }
}

// ---- URLs -----------------------------------------------------------------

// URL is a parsed http URL.
struct URL {
    Scheme string
    // host[:port] as written.
    Host string
    Hostname string
    Port int
    Path string
    RawQuery string

    // The path with the query, as sent in the request line.
    func RequestURI() string {
        if this.RawQuery == "" {
            return this.Path
        }
        return this.Path + "?" + this.RawQuery
    }

    func String() string {
        return this.Scheme + "://" + this.Host + this.RequestURI()
    }
}

// ParseURL parses "http://host[:port][/path][?query]" (a fragment is dropped).
func ParseURL(raw string) URL! {
    var i = std.strings.Index(raw, "://")
    if i <= 0 {
        return error("invalid URL \"" + raw + "\": missing scheme")
    }
    var u = URL{Scheme: std.strings.ToLower(raw[..i])}
    var rest = raw[i + 3..]
    var h = std.strings.Index(rest, "#")
    if h >= 0 {
        rest = rest[..h]
    }
    var slash = std.strings.Index(rest, "/")
    var q = std.strings.Index(rest, "?")
    var end = len(rest)
    if slash >= 0 {
        end = slash
    }
    if q >= 0 && q < end {
        end = q
    }
    u.Host = rest[..end]
    rest = rest[end..]
    q = std.strings.Index(rest, "?")
    if q >= 0 {
        u.RawQuery = rest[q + 1..]
        rest = rest[..q]
    }
    u.Path = rest
    if u.Path == "" {
        u.Path = "/"
    }
    if u.Host == "" {
        return error("invalid URL \"" + raw + "\": missing host")
    }
    var defPort = 80
    if u.Scheme == "https" {
        defPort = 443
    }
    var c = std.strings.LastIndex(u.Host, ":")
    var b = std.strings.LastIndex(u.Host, "]")
    if c > b {
        u.Hostname = u.Host[..c]
        u.Port = try std.strings.ToInt(u.Host[c + 1..]) else -1
        if u.Port < 0 || u.Port > 65535 {
            return error("invalid URL \"" + raw + "\": bad port")
        }
    } else {
        u.Hostname = u.Host
        u.Port = defPort
    }
    if std.strings.HasPrefix(u.Hostname, "[") && std.strings.HasSuffix(u.Hostname, "]") {
        u.Hostname = u.Hostname[1..len(u.Hostname) - 1]
    }
    return u
}

// The target of a redirect from u to location.
func resolveRef(u URL, location string) string {
    if std.strings.Contains(location, "://") {
        return location
    }
    var base = u.Scheme + "://" + u.Host
    if std.strings.HasPrefix(location, "/") {
        return base + location
    }
    var dir = u.Path
    var k = std.strings.LastIndex(dir, "/")
    if k >= 0 {
        dir = dir[..k + 1]
    }
    return base + dir + location
}

// ---- client ---------------------------------------------------------------

// Client sends requests. Timeout (milliseconds, 0 = none) applies to the
// connect and to every network read and write; MaxRedirects (default 10
// when 0, negative = do not follow) limits followed redirects.
struct Client {
    Timeout int
    MaxRedirects int
    // Extra headers sent with every request.
    Headers map[string]string
    // TLS settings for https URLs (nil: system roots, SNI and host name
    // from the URL, ALPN http/1.1).
    TLS *std.net.tls.Config

    func Get(url string) Task<Response!> {
        return doRequest(this.TLS, this.Timeout, this.MaxRedirects, this.Headers, "GET", url, "", "")
    }

    func Post(url string, body string, contentType string) Task<Response!> {
        return doRequest(this.TLS, this.Timeout, this.MaxRedirects, this.Headers, "POST", url, body, contentType)
    }

    // Any method; headers may be nil.
    func Do(method string, url string, body string, headers map[string]string) Task<Response!> {
        var h = map[string]string{}
        for k, v in this.Headers {
            h[k] = v
        }
        for k, v in headers {
            h[k] = v
        }
        return doRequest(this.TLS, this.Timeout, this.MaxRedirects, h, method, url, body, "")
    }
}

// Get fetches url with a default client (30 s timeout, 10 redirects).
func Get(url string) Task<Response!> {
    return doRequest(nil, 30000, 0, nil, "GET", url, "", "")
}

// Post sends body with the given Content-Type.
func Post(url string, body string, contentType string) Task<Response!> {
    return doRequest(nil, 30000, 0, nil, "POST", url, body, contentType)
}

async func doRequest(tc *std.net.tls.Config, timeout int, maxRedirects int, headers map[string]string, method string, url string, body string, contentType string) Response! {
    var limit = maxRedirects
    if limit == 0 {
        limit = 10
    }
    var m = method
    var target = url
    var payload = body
    var ctype = contentType
    var redirects = 0
    for {
        var u = ParseURL(target)?
        var res = await roundTrip(tc, timeout, headers, m, u, payload, ctype)?
        var loc = res.Header("Location")
        var st = res.Status
        var isRedirect = st == 301 || st == 302 || st == 303 || st == 307 || st == 308
        if !isRedirect || loc == "" || limit < 0 {
            return res
        }
        redirects += 1
        if redirects > limit {
            return error("GET " + url + ": stopped after " + itoa(limit) + " redirects")
        }
        if st == 303 || ((st == 301 || st == 302) && m == "POST") {
            m = "GET"
            payload = ""
            ctype = ""
        }
        target = resolveRef(u, loc)
    }
}

// Connects to the host of u: plain TCP for http, TLS for https.
async func dialStream(tc *std.net.tls.Config, u URL, timeout int) stream! {
    var addr = std.net.JoinHostPort(u.Hostname, u.Port)
    if u.Scheme == "https" {
        var cfg = std.net.tls.Config{}
        if tc != nil {
            cfg = *tc
        }
        if cfg.ServerName == "" {
            cfg.ServerName = u.Hostname
        }
        if len(cfg.ALPN) == 0 {
            cfg.ALPN = ["http/1.1"]
        }
        var tlsConn = await std.net.tls.DialTimeout(addr, cfg, timeout)?
        return tlsConn
    }
    var c = await std.net.DialTimeout(addr, timeout)?
    return c
}

async func roundTrip(tc *std.net.tls.Config, timeout int, headers map[string]string, method string, u URL, body string, contentType string) Response! {
    if u.Scheme != "http" && u.Scheme != "https" {
        return error(method + " " + u.String() + ": unsupported URL scheme \"" + u.Scheme + "\"")
    }
    var c = try await dialStream(tc, u, timeout) catch err {
        return std.errors.Wrap(err, method + " " + u.String())
    }
    c.SetTimeout(timeout)
    var req = method + " " + u.RequestURI() + " HTTP/1.1\r\nHost: " + u.Host + "\r\n"
    var sentUA = false
    var sentAccept = false
    for k, v in headers {
        var ck = CanonicalHeader(k)
        if ck == "User-Agent" {
            sentUA = true
        }
        if ck == "Accept" {
            sentAccept = true
        }
        if ck != "Host" && ck != "Content-Length" && ck != "Connection" {
            req += ck + ": " + v + "\r\n"
        }
    }
    if !sentUA {
        req += "User-Agent: slang-http/0.1\r\n"
    }
    if !sentAccept {
        req += "Accept: */*\r\n"
    }
    // std.diag: W3C trace context of the current span.
    var traceparent = std.diag.Traceparent()
    if traceparent != "" && headers["traceparent"] == "" && headers["Traceparent"] == "" {
        req += "traceparent: " + traceparent + "\r\n"
    }
    if contentType != "" {
        req += "Content-Type: " + contentType + "\r\n"
    }
    if body != "" || method == "POST" || method == "PUT" || method == "PATCH" {
        req += "Content-Length: " + itoa(len(body)) + "\r\n"
    }
    req += "Connection: close\r\n\r\n" + body
    _ = try await c.WriteString(req) catch err {
        c.Close()
        return std.errors.Wrap(err, method + " " + u.String())
    }
    var r = new reader{c: c}
    var res = try await readResponse(r, method) catch err {
        c.Close()
        return std.errors.Wrap(err, method + " " + u.String())
    }
    c.Close()
    return res
}

async func readResponse(r *reader, method string) Response! {
    for {
        var head = await r.readHead(1 << 20)?
        if head == "" {
            return error("server closed the connection without a response")
        }
        var lines = std.strings.Split(head, "\r\n")
        var sl = std.strings.SplitN(lines[0], " ", 3)
        if len(sl) < 2 || !std.strings.HasPrefix(sl[0], "HTTP/") {
            return error("malformed status line \"" + lines[0] + "\"")
        }
        var status = try std.strings.ToInt(sl[1]) else -1
        if status < 100 || status > 999 {
            return error("malformed status line \"" + lines[0] + "\"")
        }
        var h = parseHeaders(lines)?
        if status >= 100 && status < 200 {
            continue
        }
        var res = Response{Status: status, Headers: h}
        if method == "HEAD" || status == 204 || status == 304 {
            return res
        }
        if headerHas(h, "Transfer-Encoding", "chunked") {
            res.Body = await r.readChunked()?
        } else if h["Content-Length"] != "" {
            var n = try std.strings.ToInt(h["Content-Length"]) else -1
            if n < 0 {
                return error("invalid Content-Length \"" + h["Content-Length"] + "\"")
            }
            res.Body = await r.readN(n)?
        } else {
            res.Body = await r.readAll()?
        }
        return res
    }
}

// ---- server ---------------------------------------------------------------

// Handler serves requests. Handler methods are not async (interfaces
// cannot declare async methods); they return the task of an async call,
// e.g. `func Serve(req, res) Task<void!> { return this.serve(req, res) }`.
interface Handler {
    func Serve(req *std.net.http.Request, res *std.net.http.Response) Task<void!>
}

struct route {
    method string
    parts []string
    handler func(*Request, *Response) Task<void!>
}

// Router dispatches by method and path pattern; "{name}" segments match
// any one segment and are stored in req.Params, a final "*" matches the
// rest of the path. Unknown paths get 404, other methods 405.
struct Router {
    routes []route

    func Handle(method string, pattern string, h func(*Request, *Response) Task<void!>) {
        this.routes = append(this.routes, route{method: method, parts: splitPath(pattern), handler: h})
    }

    func Get(pattern string, h func(*Request, *Response) Task<void!>) {
        this.Handle("GET", pattern, h)
    }

    func Post(pattern string, h func(*Request, *Response) Task<void!>) {
        this.Handle("POST", pattern, h)
    }

    func Put(pattern string, h func(*Request, *Response) Task<void!>) {
        this.Handle("PUT", pattern, h)
    }

    func Delete(pattern string, h func(*Request, *Response) Task<void!>) {
        this.Handle("DELETE", pattern, h)
    }

    func Serve(req *Request, res *Response) Task<void!> {
        var parts = splitPath(req.Path)
        var allowed = ""
        for r in this.routes {
            var params = matchRoute(r.parts, parts)
            if params == nil {
                continue
            }
            if r.method != req.Method && !(r.method == "GET" && req.Method == "HEAD") {
                if allowed != "" {
                    allowed += ", "
                }
                allowed += r.method
                continue
            }
            req.Params = params
            return r.handler(req, res)
        }
        if allowed != "" {
            res.SetHeader("Allow", allowed)
            return plainStatus(res, 405)
        }
        return plainStatus(res, 404)
    }
}

// NewRouter makes an empty router.
func NewRouter() *Router {
    return new Router{}
}

async func plainStatus(res *Response, status int) void! {
    res.Text(status, itoa(status) + " " + StatusText(status) + "\n")
}

func splitPath(p string) []string {
    var out []string = []
    for s in std.strings.Split(p, "/") {
        if s != "" {
            out = append(out, s)
        }
    }
    return out
}

// The parameters if pattern matches path, else nil.
func matchRoute(pattern []string, path []string) map[string]string {
    var params = map[string]string{}
    for i, p in pattern {
        if p == "*" && i == len(pattern) - 1 {
            var rest = ""
            for j in i..len(path) {
                if j > i {
                    rest += "/"
                }
                rest += path[j]
            }
            params["*"] = rest
            return params
        }
        if i >= len(path) {
            return nil
        }
        if std.strings.HasPrefix(p, "{") && std.strings.HasSuffix(p, "}") {
            params[p[1..len(p) - 1]] = unescape(path[i])
        } else if p != path[i] {
            return nil
        }
    }
    if len(pattern) != len(path) {
        return nil
    }
    return params
}

// Server serves HTTP/1.1 on Addr ("host:port"; port 0 picks a free one).
// Every connection is a task; keep-alive connections serve several
// requests. IdleTimeout (ms, default 5000) closes connections that send
// nothing; Shutdown stops accepting, closes idle connections and waits
// for the requests in flight.
struct Server {
    Addr string
    Handler Handler
    IdleTimeout int
    // Serves HTTPS with this configuration when set (ALPN defaults to
    // http/1.1).
    TLS *std.net.tls.ServerConfig
    st *serverState

    // Binds the listening socket (Serve does it if needed).
    func Listen() void! {
        if this.st != nil {
            return
        }
        var l = std.net.Listen(this.Addr)?
        var idle = this.IdleTimeout
        if idle == 0 {
            idle = 5000
        }
        var st = new serverState{l: l, handler: this.Handler, idle: idle}
        if this.TLS != nil {
            var cfg = *this.TLS
            if len(cfg.ALPN) == 0 {
                cfg.ALPN = ["http/1.1"]
            }
            st.tlsL = try std.net.tls.Server(l, cfg) catch err {
                l.Close()
                return err
            }
            st.useTLS = true
        }
        this.st = st
    }

    // The bound port (after Listen).
    func Port() int {
        if this.st == nil {
            return 0
        }
        return this.st.l.Port()
    }

    // Accepts connections until Shutdown.
    func Serve() Task<void!> {
        try this.Listen() catch err {
            return failed(err)
        }
        return acceptLoop(this.st)
    }

    // Stops the server: no new connections, idle ones are closed, and the
    // task completes once every request in flight has been answered.
    func Shutdown() Task<void> {
        if this.st == nil {
            return std.task.Delay(0)
        }
        return shutdown(this.st)
    }
}

async func failed(err error) void! {
    return err
}

struct connEntry {
    c stream
    idle bool
}

struct serverState {
    l std.net.Listener
    useTLS bool
    tlsL std.net.tls.Listener
    handler Handler
    idle int
    guard int
    closing bool
    conns []*connEntry
    wg std.task.WaitGroup
}

// The next connection, wrapped in TLS for an HTTPS server (its handshake
// runs in the connection task, on the first read).
async func acceptStream(st *serverState) stream! {
    if st.useTLS {
        var tc = await st.tlsL.Accept()?
        return tc
    }
    var c = await st.l.Accept()?
    return c
}

async func acceptLoop(st *serverState) void! {
    for {
        var c = try await acceptStream(st) catch err {
            if st.closing {
                return
            }
            return err
        }
        var e = new connEntry{c: c, idle: true}
        std.task.rtLock(&st.guard)
        if st.closing {
            std.task.rtUnlock(&st.guard)
            c.Close()
            return
        }
        st.conns = append(st.conns, e)
        st.wg.Add(1)
        std.task.rtUnlock(&st.guard)
        _ = serveConn(st, e)
    }
}

async func shutdown(st *serverState) {
    std.task.rtLock(&st.guard)
    st.closing = true
    var conns = st.conns
    std.task.rtUnlock(&st.guard)
    st.l.Close()
    for e in conns {
        if e.idle {
            e.c.Close()
        }
    }
    await st.wg.Wait()
}

func dropConn(st *serverState, e *connEntry) {
    std.task.rtLock(&st.guard)
    var keep []*connEntry = []
    for x in st.conns {
        if x != e {
            keep = append(keep, x)
        }
    }
    st.conns = keep
    std.task.rtUnlock(&st.guard)
    e.c.Close()
    st.wg.Done()
}

async func serveConn(st *serverState, e *connEntry) {
    var r = new reader{c: e.c}
    for {
        e.c.SetTimeout(st.idle)
        var req = try await readRequest(r) catch err {
            if !e.c.IsClosed() {
                _ = await writeResponse(e.c, "HTTP/1.1", false, errorResponse(400, err.Message()), false)
            }
            break
        }
        if req == nil {
            break
        }
        e.idle = false
        req.RemoteAddr = e.c.RemoteAddr()
        var res = new Response{Status: 200}
        // std.diag: a traceparent header makes the caller's span the parent.
        var remote = std.diag.enterRemote(req.Header("traceparent"))
        try await st.handler.Serve(req, res) catch err {
            res = errorResponse(500, err.Message())
        }
        std.diag.leaveRemote(remote)
        var keep = req.Proto == "HTTP/1.1" && !headerHas(req.Headers, "Connection", "close") && !st.closing
        if req.Proto == "HTTP/1.0" && headerHas(req.Headers, "Connection", "keep-alive") && !st.closing {
            keep = true
        }
        var head = req.Method == "HEAD"
        var ok = try await writeResponse(e.c, req.Proto, keep, res, head) else false
        if !ok || !keep || st.closing {
            break
        }
        e.idle = true
    }
    dropConn(st, e)
}

func errorResponse(status int, msg string) *Response {
    var res = new Response{}
    res.Text(status, itoa(status) + " " + StatusText(status) + ": " + msg + "\n")
    return res
}

// Reads one request; nil at a clean end of stream.
async func readRequest(r *reader) *Request! {
    var head = await r.readHead(64 << 10)?
    if head == "" {
        return nil
    }
    var lines = std.strings.Split(head, "\r\n")
    var rl = std.strings.Split(lines[0], " ")
    if len(rl) != 3 || !std.strings.HasPrefix(rl[2], "HTTP/1.") {
        return error("malformed request line")
    }
    var req = new Request{Method: rl[0], Proto: rl[2]}
    var target = rl[1]
    var q = std.strings.Index(target, "?")
    if q >= 0 {
        req.Query = target[q + 1..]
        target = target[..q]
    }
    req.Path = target
    req.Headers = parseHeaders(lines)?
    if headerHas(req.Headers, "Transfer-Encoding", "chunked") {
        req.Body = await r.readChunked()?
    } else if req.Headers["Content-Length"] != "" {
        var n = try std.strings.ToInt(req.Headers["Content-Length"]) else -1
        if n < 0 {
            return error("invalid Content-Length")
        }
        req.Body = await r.readN(n)?
    }
    return req
}

async func writeResponse(c stream, proto string, keep bool, res *Response, headOnly bool) bool! {
    var status = res.Status
    if status == 0 {
        status = 200
    }
    var out = "HTTP/1.1 " + itoa(status) + " " + StatusText(status) + "\r\n"
    var hasType = false
    for k, v in res.Headers {
        var ck = CanonicalHeader(k)
        if ck == "Content-Length" || ck == "Connection" || ck == "Transfer-Encoding" {
            continue
        }
        if ck == "Content-Type" {
            hasType = true
        }
        out += ck + ": " + v + "\r\n"
    }
    if !hasType && res.Body != "" {
        out += "Content-Type: text/plain; charset=utf-8\r\n"
    }
    out += "Content-Length: " + itoa(len(res.Body)) + "\r\n"
    if keep {
        if proto == "HTTP/1.0" {
            out += "Connection: keep-alive\r\n"
        }
    } else {
        out += "Connection: close\r\n"
    }
    out += "\r\n"
    if !headOnly {
        out += res.Body
    }
    _ = await c.WriteString(out)?
    return true
}
