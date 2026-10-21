// The HTTP client (see http.sl for the file layout): Client, the request
// builder, Transport with its per-host connection pool (keep-alive, TLS
// session reuse, HTTP/2 multiplexing via ALPN), proxies (HTTP_PROXY /
// HTTPS_PROXY / NO_PROXY, CONNECT for https), transparent gzip, cookies
// (CookieJar) and streamed response bodies (BodyReader).
namespace std.net.http

// ---- the request builder -----------------------------------------------------

// ClientRequest is an outgoing request: http.NewRequest(method, url) and
// chained setters, then client.Send(req) / client.Stream(req) / req.Send().
struct ClientRequest {
    Method  string
    URL     string
    headers map[string]string
    query   []string
    body    string
    hasBody bool
    timeout int

    // Sets a header (canonical name).
    func Header(name string, value string) *ClientRequest {
        if this.headers == nil {
            this.headers = map[string]string{}
        }
        this.headers[rtCanonical(name)] = value
        return this
    }

    // Adds a query parameter (escaped) to the URL.
    func Query(name string, value string) *ClientRequest {
        this.query = append(this.query, QueryEscape(name) + "=" + QueryEscape(value))
        return this
    }

    // The body: a string, a []byte or a std.io.Reader (read to its end
    // when the request is sent).
    func Body(x any) *ClientRequest {
        this.hasBody = true
        switch x {
            case string s:
                this.body = s
            case []byte b:
                this.body = std.strings.FromBytes(b)
            case std.io.Reader r:
                this.body = try std.io.ReadAllString(r) else ""
            default:
                this.body = ""
        }
        return this
    }

    // A JSON body (Content-Type application/json).
    func JSON<T>(v T) *ClientRequest {
        this.body = std.encoding.json.Format<T>(v)
        this.hasBody = true
        return this.Header("Content-Type", "application/json")
    }

    // A form body (application/x-www-form-urlencoded) from name/value pairs.
    func Form(values map[string]string) *ClientRequest {
        var keys []string = []
        for k in values {
            keys = append(keys, k)
        }
        std.slices.Sort(keys)
        var parts []string = []
        for k in keys {
            parts = append(parts, QueryEscape(k) + "=" + QueryEscape(values[k]))
        }
        this.body = std.strings.Join(parts, "&")
        this.hasBody = true
        return this.Header("Content-Type", "application/x-www-form-urlencoded")
    }

    func ContentType(ct string) *ClientRequest {
        return this.Header("Content-Type", ct)
    }

    // A cookie for this request only.
    func Cookie(name string, value string) *ClientRequest {
        if this.headers == nil {
            this.headers = map[string]string{}
        }
        var old = this.headers["Cookie"]
        if old == "" {
            this.headers["Cookie"] = name + "=" + value
        } else {
            this.headers["Cookie"] = old + "; " + name + "=" + value
        }
        return this
    }

    // Basic authentication.
    func BasicAuth(user string, password string) *ClientRequest {
        return this.Header(
            "Authorization",
            "Basic " + std.encoding.base64.EncodeString(user + ":" + password),
        )
    }

    // A total time limit in milliseconds (connect, request, response
    // head and body), enforced through the task context: the waits of the
    // request fail with task.ErrCancelled when it expires.
    func Timeout(ms int) *ClientRequest {
        this.timeout = ms
        return this
    }

    // Sends with a default client.
    func Send() Task<Response!> {
        var c = Client{}
        return c.Send(this)
    }

    func fullURL() string {
        if len(this.query) == 0 {
            return this.URL
        }
        var sep = "?"
        if std.strings.Contains(this.URL, "?") {
            sep = "&"
        }
        return this.URL + sep + std.strings.Join(this.query, "&")
    }
}

// A request to build with the ClientRequest setters.
func NewRequest(method string, url string) *ClientRequest {
    return new ClientRequest{Method: std.strings.ToUpper(method), URL: url}
}

// ---- client -------------------------------------------------------------------

// Client sends requests. Timeout (milliseconds, 0 = none) applies to the
// connect and to every network read and write; TotalTimeout limits a whole
// request (task-context deadline). MaxRedirects (default 10 when 0,
// negative = do not follow) limits followed redirects. Connections are
// pooled by the Transport (the process-wide default one when nil); Jar
// stores and sends cookies.
struct Client {
    Timeout      int
    TotalTimeout int
    MaxRedirects int
    // Extra headers sent with every request.
    Headers map[string]string
    // TLS settings for https URLs (nil: system roots, SNI and host name
    // from the URL); overrides Transport.TLS.
    TLS       *std.net.tls.Config
    Transport *Transport
    Jar       *CookieJar

    func Get(url string) Task<Response!> {
        return this.Do("GET", url, "", nil)
    }

    func Post(url string, body string, contentType string) Task<Response!> {
        var r = NewRequest("POST", url).Body(body)
        if contentType != "" {
            r.ContentType(contentType)
        }
        return this.Send(r)
    }

    // Any method; headers may be nil.
    func Do(method string, url string, body string, headers map[string]string) Task<Response!> {
        var r = NewRequest(method, url)
        for k, v in headers {
            r.Header(k, v)
        }
        if body != "" {
            r.Body(body)
        }
        return this.Send(r)
    }

    // Sends a built request and reads the whole response body.
    func Send(req *ClientRequest) Task<Response!> {
        return this.start(req, false)
    }

    // Like Send, but the body is left on the connection: read it from
    // res.Reader (an io.AsyncReader) and Close it when done.
    func Stream(req *ClientRequest) Task<Response!> {
        return this.start(req, true)
    }

    func start(req *ClientRequest, stream bool) Task<Response!> {
        var total = req.timeout
        if total == 0 {
            total = this.TotalTimeout
        }
        var cl = *this
        if total > 0 {
            return std.task.WithTimeout(total, func() Task<Response!> {
                return send(cl, req, stream)
            })
        }
        return send(cl, req, stream)
    }
}

// Get fetches url with a default client (30 s timeout, 10 redirects).
func Get(url string) Task<Response!> {
    var c = Client{Timeout: 30000}
    return c.Get(url)
}

// Post sends body with the given Content-Type.
func Post(url string, body string, contentType string) Task<Response!> {
    var c = Client{Timeout: 30000}
    return c.Post(url, body, contentType)
}

// GET url and decode the JSON body as T (a non-2xx status is an error).
async func GetJSON<T>(url string) T! {
    var res = await Get(url)?
    if res.Status < 200 || res.Status > 299 {
        return error("GET " + url + ": " + itoa(res.Status) + " " + StatusText(res.Status))
    }
    return std.encoding.json.Parse<T>(res.Body)?
}

func isRedirect(st int) bool {
    return st == 301 || st == 302 || st == 303 || st == 307 || st == 308
}

async func send(cl Client, req *ClientRequest, stream bool) Response! {
    var t = cl.Transport
    if t == nil {
        t = DefaultTransport()
    }
    var limit = cl.MaxRedirects
    if limit == 0 {
        limit = 10
    }
    var method = req.Method
    if method == "" {
        method = "GET"
    }
    var target = req.fullURL()
    var body = req.body
    var hasBody = req.hasBody
    var headers = map[string]string{}
    for k, v in cl.Headers {
        headers[rtCanonical(k)] = v
    }
    for k, v in req.headers {
        headers[k] = v
    }
    var redirects = 0
    for {
        var u = ParseURL(target)?
        var h = headers
        if cl.Jar != nil {
            var cs = cl.Jar.Cookies(u)
            if len(cs) > 0 {
                h = map[string]string{}
                for k, v in headers {
                    h[k] = v
                }
                var line = ""
                for c in cs {
                    if line != "" {
                        line += "; "
                    }
                    line += c.Name + "=" + c.Value
                }
                if h["Cookie"] != "" {
                    line = h["Cookie"] + "; " + line
                }
                h["Cookie"] = line
            }
        }
        var res = try await t.roundTrip(
            cl,
            method,
            u,
            h,
            body,
            hasBody,
            stream || (limit >= 0),
        ) catch err {
            return std.errors.Wrap(err, method + " " + u.String())
        }
        if cl.Jar != nil && res.Headers["Set-Cookie"] != "" {
            var got []Cookie = []
            var now = std.time.Now() / 1000
            for line in std.strings.Split(res.Headers["Set-Cookie"], "\n") {
                var c = try ParseSetCookie(line, now) else Cookie{}
                if c.Name != "" {
                    got = append(got, c)
                }
            }
            cl.Jar.SetCookies(u, got)
        }
        var loc = res.Headers["Location"]
        var follow = isRedirect(res.Status) && loc != "" && limit >= 0
        if !follow {
            if !stream && res.Reader != nil {
                try {
                    res.Body = await res.Reader.ReadAll()?
                } catch err {
                    return std.errors.Wrap(err, method + " " + u.String())
                }
                res.Reader = nil
            }
            return res
        }
        if res.Reader != nil {
            // drain (small) redirect bodies so the connection can be reused
            _ = try await res.Reader.ReadAll() else ""
            res.Reader = nil
        }
        redirects += 1
        if redirects > limit {
            return error(method + " " + req.URL + ": stopped after " + itoa(limit) + " redirects")
        }
        var st = res.Status
        if st == 303 || ((st == 301 || st == 302) && method == "POST") {
            if method != "HEAD" {
                method = "GET"
            }
            body = ""
            hasBody = false
            delete(headers, "Content-Type")
        }
        var next = resolveRef(u, loc)
        var nu = try ParseURL(next) else u
        if nu.Host != u.Host {
            // credentials stay with their host
            delete(headers, "Authorization")
            delete(headers, "Cookie")
        }
        target = next
    }
}

// ---- the transport ---------------------------------------------------------------

// Transport opens, pools and reuses connections. The zero value works;
// the process-wide DefaultTransport() is used by clients without one.
struct Transport {
    // Idle keep-alive connections kept per host (default 8).
    MaxIdleConnsPerHost int
    // Connections per host in total, in use or idle (0: no limit); more
    // requests wait for one to be free.
    MaxConnsPerHost int
    // Milliseconds an idle connection is kept (default 90000).
    IdleConnTimeout int
    // Milliseconds for the TCP connect (default: Client.Timeout, else 30000).
    DialTimeout int
    // Milliseconds for the TLS handshake (default 10000).
    TLSHandshakeTimeout int
    // Milliseconds to wait for the response head after the request was
    // written (0: Client.Timeout).
    ResponseHeaderTimeout int
    // TLS settings for https (Client.TLS wins); sessions are resumed from
    // the transport's session cache.
    TLS *std.net.tls.Config
    // Proxy URL ("http://host:port", user:password@ allowed) for every
    // request; "" with ProxyFromEnvironment reads HTTP_PROXY, HTTPS_PROXY
    // and NO_PROXY (and their lower-case forms).
    Proxy                string
    ProxyFromEnvironment bool
    DisableKeepAlives    bool
    // Do not ask for gzip (Accept-Encoding) nor decode it.
    DisableCompression bool
    // Do not offer h2 over TLS.
    DisableHTTP2 bool
    // Speak HTTP/2 with prior knowledge to http:// URLs (h2c).
    H2C bool
    p   *pool

    func state() *pool {
        // Created on first use; concurrent first requests must share one
        // pool (a second one would be dropped, closing its connections).
        std.diag.rtLock()
        if this.p == nil {
            this.p = new pool{
                hosts: map[string]*hostPool{},
                sessions: std.net.tls.NewSessionCache(),
            }
        }
        var p = this.p
        std.diag.rtUnlock()
        return p
    }

    // Closes the idle connections.
    func CloseIdleConnections() {
        var p = this.state()
        std.task.rtLock(&p.guard)
        var all []*pconn = []
        var idleH2 []*h2conn = []
        for _, hp in p.hosts {
            for pc in hp.idle {
                all = append(all, pc)
                hp.active -= 1
            }
            hp.idle = []
            // an HTTP/2 connection without streams is idle too
            if hp.h2 != nil && h2Idle(hp.h2) {
                idleH2 = append(idleH2, hp.h2)
                hp.h2 = nil
                hp.active -= 1
            }
        }
        std.task.rtUnlock(&p.guard)
        for pc in all {
            pc.c.close()
        }
        for h in idleH2 {
            h2CloseIdle(h)
        }
    }

    // Idle connections in the pool (all hosts) and connections open.
    func Stats() PoolStats {
        var p = this.state()
        std.task.rtLock(&p.guard)
        var s = PoolStats{}
        for _, hp in p.hosts {
            s.Idle += len(hp.idle)
            s.Open += hp.active
            if hp.h2 != nil {
                s.HTTP2 += 1
            }
        }
        s.Dials = p.dials
        s.Reused = p.reused
        std.task.rtUnlock(&p.guard)
        return s
    }

    func roundTrip(
        cl Client,
        method string,
        u URL,
        h map[string]string,
        body string,
        hasBody bool,
        buffered bool,
    ) Task<Response!> {
        return transportRoundTrip(this, cl, method, u, h, body, hasBody, buffered)
    }
}

// Pool counters (Transport.Stats).
struct PoolStats {
    Idle  int
    Open  int
    HTTP2 int
    // Connections dialed and requests sent on a reused connection.
    Dials  int
    Reused int
}

struct defaultTransportHolder {
    t *Transport
}

// The process-wide transport of clients without one (and of http.Get).
func DefaultTransport() *Transport {
    std.diag.rtLock()
    var m = std.diag.rtSlots()
    if m == nil {
        m = map[string]any{}
        std.diag.rtSetSlots(m)
    }
    switch m["http.transport"] {
        case defaultTransportHolder h:
            std.diag.rtUnlock()
            return h.t
    }
    var t = new Transport{ProxyFromEnvironment: true}
    m["http.transport"] = defaultTransportHolder{t: t}
    std.diag.rtUnlock()
    return t
}

struct pool {
    guard    int
    hosts    map[string]*hostPool
    sessions *std.net.tls.SessionCache
    dials    int
    reused   int

    func Drop() {
        closePool(this)
    }
}

struct hostPool {
    idle []*pconn
    // connections open for this key (idle or in use)
    active  int
    waiters []Task<void>
    // the shared HTTP/2 connection, when the host speaks h2
    h2 *h2conn
    // an h2 dial is in progress (requests wait for it instead of dialing)
    dialing bool
    // the host answered h2 before
    knownH2 bool
}

// A pooled connection.
struct pconn {
    c         *conn
    b         *rbuf
    key       string
    idleSince int
    reused    bool
    // the request goes in absolute form to an HTTP proxy
    viaProxy bool
    h2       *h2conn
}

// A pool going away (its Transport was dropped) closes its connections,
// which also ends the readers of its HTTP/2 connections.
func closePool(p *pool) {
    for _, hp in p.hosts {
        for pc in hp.idle {
            pc.c.close()
        }
        if hp.h2 != nil {
            // streams still running finish first (h2forget closes it)
            h2CloseWhenIdle(hp.h2)
        }
    }
}

// Milliseconds an idle connection is kept (Transport.IdleConnTimeout).
func idleTimeoutOf(t *Transport) int {
    if t.IdleConnTimeout <= 0 {
        return 90000
    }
    return t.IdleConnTimeout
}

func poolKey(u URL, proxy string) string {
    return proxy + "|" + u.Scheme + "://" + u.Addr()
}

func hostOf(p *pool, key string) *hostPool {
    var hp = p.hosts[key]
    if hp == nil {
        hp = new hostPool{}
        p.hosts[key] = hp
    }
    return hp
}

func wakeAll(hp *hostPool) {
    var ws = hp.waiters
    hp.waiters = []
    for w in ws {
        std.task.rtFire(w)
    }
}

func wakeOne(hp *hostPool) {
    if len(hp.waiters) > 0 {
        var s = hp.waiters[0]
        var none Task<void>
        hp.waiters[0] = none
        hp.waiters = hp.waiters[1..]
        std.task.rtFire(s)
    }
}

// A connection for key: an idle one (newest first, expired ones closed),
// a new one, or the host's HTTP/2 connection; waits while
// MaxConnsPerHost connections are in use.
async func getConn(t *Transport, cl Client, u URL, proxy string) *pconn! {
    var p = t.state()
    var key = poolKey(u, proxy)
    var idleTimeout = t.IdleConnTimeout
    if idleTimeout <= 0 {
        idleTimeout = 90000
    }
    for {
        var stale []*pconn = []
        var found *pconn
        var mayDial = false
        var wait Task<void>
        var waiting = false
        std.task.rtLock(&p.guard)
        var hp = hostOf(p, key)
        if hp.h2 != nil {
            if h2Usable(hp.h2) {
                var pc = new pconn{c: hp.h2.c, key: key, h2: hp.h2, reused: true}
                p.reused += 1
                std.task.rtUnlock(&p.guard)
                return pc
            }
            // closed or draining (GOAWAY): it finishes on its own
            hp.h2 = nil
            hp.active -= 1
        }
        var now = std.time.Now()
        for found == nil && len(hp.idle) > 0 {
            var pc = hp.idle[len(hp.idle) - 1]
            hp.idle = hp.idle[..len(hp.idle) - 1]
            if now - pc.idleSince > idleTimeout || pc.c.isClosed() {
                stale = append(stale, pc)
                hp.active -= 1
            } else {
                found = pc
            }
        }
        var h2Likely = (t.H2C && proxy == "" && u.Scheme == "http") || hp.knownH2
        if found != nil {
            p.reused += 1
        } else if hp.dialing && h2Likely {
            // one HTTP/2 connection serves everyone: wait for it
            wait = std.task.rtWaker()
            hp.waiters = append(hp.waiters, wait)
            waiting = true
        } else if t.MaxConnsPerHost <= 0 || hp.active < t.MaxConnsPerHost {
            hp.active += 1
            p.dials += 1
            mayDial = true
            if h2Likely {
                hp.dialing = true
            }
        } else {
            wait = std.task.rtWaker()
            hp.waiters = append(hp.waiters, wait)
            waiting = true
        }
        std.task.rtUnlock(&p.guard)
        for pc in stale {
            pc.c.close()
        }
        if found != nil {
            found.reused = true
            return found
        }
        if mayDial {
            var pc = try await dialConn(t, cl, u, proxy) catch err {
                std.task.rtLock(&p.guard)
                hp.active -= 1
                hp.dialing = false
                wakeAll(hp)
                std.task.rtUnlock(&p.guard)
                return err
            }
            pc.key = key
            if pc.h2 == nil {
                std.task.rtLock(&p.guard)
                if hp.dialing {
                    hp.dialing = false
                    hp.knownH2 = false
                    wakeAll(hp)
                }
                std.task.rtUnlock(&p.guard)
            }
            if pc.h2 != nil {
                var extra = false
                std.task.rtLock(&p.guard)
                hp.dialing = false
                hp.knownH2 = true
                wakeAll(hp)
                if hp.h2 == nil {
                    hp.h2 = pc.h2
                } else if !h2Usable(hp.h2) {
                    hp.h2 = pc.h2
                    hp.active -= 1
                } else {
                    // another request raced us to it: use the first one
                    extra = true
                    hp.active -= 1
                }
                var shared = hp.h2
                std.task.rtUnlock(&p.guard)
                if extra {
                    pc.c.close()
                    return new pconn{c: shared.c, key: key, h2: shared, reused: true}
                }
            }
            return pc
        }
        if waiting {
            await wait
        }
    }
}

// Returns a connection after a complete exchange (keep: it may be reused).
func putConn(t *Transport, pc *pconn, keep bool) {
    if pc.h2 != nil {
        return
    }
    var p = t.state()
    var max = t.MaxIdleConnsPerHost
    if max <= 0 {
        max = 8
    }
    var drop = !keep || t.DisableKeepAlives || pc.c.isClosed()
    std.task.rtLock(&p.guard)
    var hp = hostOf(p, pc.key)
    if !drop && len(hp.idle) < max {
        pc.idleSince = std.time.Now()
        hp.idle = append(hp.idle, pc)
    } else {
        drop = true
        hp.active -= 1
    }
    wakeOne(hp)
    std.task.rtUnlock(&p.guard)
    if drop {
        pc.c.close()
    }
}

// Forgets a broken connection.
func discardConn(t *Transport, pc *pconn) {
    pc.c.close()
    if pc.h2 != nil {
        return
    }
    var p = t.state()
    std.task.rtLock(&p.guard)
    var hp = hostOf(p, pc.key)
    hp.active -= 1
    wakeOne(hp)
    std.task.rtUnlock(&p.guard)
}

// The proxy for u ("" if none).
func proxyFor(t *Transport, u URL) string {
    if t.Proxy != "" {
        return t.Proxy
    }
    if !t.ProxyFromEnvironment {
        return ""
    }
    var p = ""
    if u.Scheme == "https" || u.Scheme == "wss" {
        p = envOr("HTTPS_PROXY", "https_proxy")
    } else {
        p = envOr("HTTP_PROXY", "http_proxy")
    }
    if p == "" {
        return ""
    }
    if noProxy(envOr("NO_PROXY", "no_proxy"), u) {
        return ""
    }
    if !std.strings.Contains(p, "://") {
        p = "http://" + p
    }
    return p
}

func envOr(a string, b string) string {
    var v = std.os.Getenv(a)
    if v == "" {
        v = std.os.Getenv(b)
    }
    return v
}

// NO_PROXY matching: "*", host names (a leading "." or not: the name and
// its subdomains), IP addresses, optional ":port".
func noProxy(list string, u URL) bool {
    if list == "" {
        return false
    }
    var host = std.strings.ToLower(u.Hostname)
    for raw in std.strings.Split(list, ",") {
        var e = std.strings.ToLower(std.strings.Trim(raw))
        if e == "" {
            continue
        }
        if e == "*" {
            return true
        }
        var c = std.strings.LastIndex(e, ":")
        if c > 0 && !std.strings.Contains(e[..c], ":") {
            if e[c + 1..] != itoa(u.Port) {
                continue
            }
            e = e[..c]
        }
        e = std.strings.TrimPrefix(e, ".")
        if host == e || std.strings.HasSuffix(host, "." + e) {
            return true
        }
    }
    return false
}

// Opens a connection for u (through proxy if set): TCP, CONNECT tunnel,
// TLS with ALPN; an "h2" result starts an HTTP/2 client connection.
async func dialConn(t *Transport, cl Client, u URL, proxy string) *pconn! {
    var dialMs = t.DialTimeout
    if dialMs <= 0 {
        dialMs = cl.Timeout
    }
    if dialMs <= 0 {
        dialMs = 30000
    }
    var tlsMs = t.TLSHandshakeTimeout
    if tlsMs <= 0 {
        tlsMs = 10000
    }
    var secure = u.Scheme == "https" || u.Scheme == "wss"
    var raw std.net.Conn
    var viaProxy = false
    if proxy != "" {
        var pu = ParseURL(proxy)?
        raw = await std.net.DialTimeout(pu.Addr(), dialMs)?
        if secure {
            await connectTunnel(raw, pu, u, dialMs)?
        } else {
            viaProxy = true
        }
    } else {
        raw = await std.net.DialTimeout(u.Addr(), dialMs)?
    }
    if !secure {
        var c = plainConn(raw)
        var pc = new pconn{c: c, b: newRbuf(4096), viaProxy: viaProxy}
        if t.H2C && !viaProxy {
            pc.h2 = await startH2Client(c, newRbuf(16384), idleTimeoutOf(t))?
        }
        return pc
    }
    var cfg = std.net.tls.Config{}
    if cl.TLS != nil {
        cfg = *cl.TLS
    } else if t.TLS != nil {
        cfg = *t.TLS
    }
    if cfg.ServerName == "" {
        cfg.ServerName = u.Hostname
    }
    if len(cfg.ALPN) == 0 {
        if t.DisableHTTP2 {
            cfg.ALPN = ["http/1.1"]
        } else {
            cfg.ALPN = ["h2", "http/1.1"]
        }
    }
    if cfg.Sessions == nil {
        cfg.Sessions = t.state().sessions
    }
    var tc = try await std.net.tls.Client(raw, cfg) catch err {
        raw.Close()
        return err
    }
    var hs = tc.Handshake()
    if !await std.task.Timeout(hs, tlsMs) {
        tc.Close()
        return error("TLS handshake timeout")
    }
    try await hs catch err {
        tc.Close()
        return err
    }
    var c = tlsConn(tc)
    var pc = new pconn{c: c, b: newRbuf(4096)}
    if tc.ConnectionState().ALPN == "h2" {
        pc.h2 = await startH2Client(c, newRbuf(16384), idleTimeoutOf(t))?
    }
    return pc
}

// CONNECT host:port through an HTTP proxy (for https).
async func connectTunnel(raw std.net.Conn, pu URL, u URL, ms int) void! {
    var addr = u.Addr()
    var req = "CONNECT " + addr + " HTTP/1.1\r\nHost: " + addr + "\r\n"
    if pu.User != "" {
        req += "Proxy-Authorization: Basic " +
            std.encoding.base64.EncodeString(unescape(pu.User)) +
            "\r\n"
    }
    req += "\r\n"
    raw.SetTimeout(ms)
    _ = try await raw.WriteString(req) catch err {
        raw.Close()
        return err
    }
    var c = plainConn(raw)
    var b = newRbuf(4096)
    var res = try await readHead(c, b) catch err {
        raw.Close()
        return std.errors.Wrap(err, "proxy CONNECT")
    }
    if res.Status != 200 {
        raw.Close()
        return error(
            "proxy CONNECT " + addr + ": " + itoa(res.Status) + " " + StatusText(res.Status),
        )
    }
    if b.buffered() > 0 {
        raw.Close()
        return error("proxy CONNECT " + addr + ": unexpected data after the response")
    }
    raw.SetTimeout(0)
}

func idempotent(method string, h map[string]string) bool {
    return method == "GET" ||
        method == "HEAD" ||
        method == "OPTIONS" ||
        method == "TRACE" ||
        method == "PUT" ||
        method == "DELETE" ||
        h["Idempotency-Key"] != ""
}

// The request head (and body) of an HTTP/1.1 exchange.
func formatRequest(
    method string,
    u URL,
    viaProxy bool,
    h map[string]string,
    body string,
    hasBody bool,
    t *Transport,
    gz bool,
    proxy string,
) string {
    var target = u.RequestURI()
    if viaProxy {
        target = u.String()
    }
    var line = method + " " + target + " HTTP/1.1\r\nHost: " + u.Host
    if h["User-Agent"] == "" {
        line += "\r\nUser-Agent: slang-http/0.1"
    }
    if h["Accept"] == "" {
        line += "\r\nAccept: */*"
    }
    if gz {
        line += "\r\nAccept-Encoding: gzip"
    }
    // std.diag: W3C trace context of the current span.
    var traceparent = std.diag.Traceparent()
    if traceparent != "" && h["Traceparent"] == "" {
        line += "\r\nTraceparent: " + traceparent
    }
    if viaProxy && proxy != "" {
        var pu = try ParseURL(proxy) else URL{}
        if pu.User != "" {
            line += "\r\nProxy-Authorization: Basic " +
                std.encoding.base64.EncodeString(unescape(pu.User))
        }
    }
    var flags = 128
    var sendLen = hasBody || body != "" || method == "POST" || method == "PUT" || method == "PATCH"
    if !sendLen {
        flags = flags | 32
    }
    if t.DisableKeepAlives {
        flags = flags | 2
    }
    var hh = h
    if h["Host"] != "" {
        hh = map[string]string{}
        for k, v in h {
            if k != "Host" {
                hh[k] = v
            }
        }
    }
    return rtFormatHead(0, line, hh, len(body), flags, body)
}

async func transportRoundTrip(
    t *Transport,
    cl Client,
    method string,
    u URL,
    h map[string]string,
    body string,
    hasBody bool,
    buffered bool,
) Response! {
    if u.Scheme != "http" && u.Scheme != "https" {
        return error("unsupported URL scheme \"" + u.Scheme + "\"")
    }
    var proxy = proxyFor(t, u)
    var gz = !t.DisableCompression &&
        h["Accept-Encoding"] == "" &&
        h["Range"] == "" &&
        method != "HEAD"
    var attempt = 0
    for {
        attempt += 1
        var pc = await getConn(t, cl, u, proxy)?
        if pc.h2 != nil {
            var res = try await h2RoundTrip(pc.h2, method, u, h, body, hasBody, gz) catch err {
                if attempt == 1 && pc.reused && h2Retryable(err) {
                    continue
                }
                return err
            }
            return res
        }
        pc.c.setTimeout(cl.Timeout)
        var out = formatRequest(method, u, pc.viaProxy, h, body, hasBody, t, gz, proxy)
        var res Response
        var failed error
        try {
            _ = await pc.c.writeString(out)?
            if t.ResponseHeaderTimeout > 0 {
                pc.c.setTimeout(t.ResponseHeaderTimeout)
            }
            res = await readHead(pc.c, pc.b)?
            pc.c.setTimeout(cl.Timeout)
        } catch err {
            failed = err
        }
        if failed != nil {
            discardConn(t, pc)
            // a reused connection the server had closed: try a fresh one
            if attempt == 1 && pc.reused && idempotent(method, h) && staleError(failed) {
                continue
            }
            return failed
        }
        var noBody = method == "HEAD" || res.Status == 204 || res.Status == 304
        var keep = !t.DisableKeepAlives && !headerHas(res.Headers, "Connection", "close")
        if res.Proto == "HTTP/1.0" && !headerHas(res.Headers, "Connection", "keep-alive") {
            keep = false
        }
        var br = new BodyReader{t: t, pc: pc, keep: keep}
        if noBody {
            br.mode = 3
        } else if headerHas(res.Headers, "Transfer-Encoding", "chunked") {
            br.mode = 1
        } else if res.Headers["Content-Length"] != "" {
            var n = rtAtoi(res.Headers["Content-Length"])
            if n < 0 {
                discardConn(t, pc)
                return error("invalid Content-Length \"" + res.Headers["Content-Length"] + "\"")
            }
            br.mode = 0
            br.left = n
        } else {
            br.mode = 2
            br.keep = false
        }
        if gz && res.Headers["Content-Encoding"] == "gzip" {
            br.gz = std.compress.gzip.NewReader()
            delete(res.Headers, "Content-Encoding")
            delete(res.Headers, "Content-Length")
        }
        if br.mode == 3 || (br.mode == 0 && br.left == 0 && br.gz == nil) {
            br.finish()
            if br.mode == 0 && !noBody {
                br.mode = 3
            }
            return res
        }
        // fast path: a Content-Length body that is already buffered
        if buffered && br.mode == 0 && br.gz == nil && pc.b.buffered() >= br.left {
            res.Body = pc.b.take(br.left)
            br.left = 0
            br.finish()
            return res
        }
        res.Reader = br
        return res
    }
}

// Errors that mean "the server closed this idle connection" (retry).
func staleError(err error) bool {
    if std.errors.Is(err, std.net.ErrReset) ||
        std.errors.Is(err, std.net.ErrEOF) ||
        std.errors.Is(err, std.net.ErrClosed) {
        return true
    }
    var m = err.Message()
    return std.strings.Contains(m, "closed the connection without a response") ||
        std.strings.Contains(m, "unexpected end of stream")
}

// Reads the next response head (1xx responses other than 101 skipped).
async func readHead(c *conn, b *rbuf) Response! {
    for {
        var end = rtHeadEnd(b.buf, b.scan, b.w)
        for end < 0 {
            if b.buffered() > 1 << 20 {
                return error("response header too large")
            }
            b.scan = maxInt(b.r, b.w - 3)
            if !await fill(c, b)? {
                if b.buffered() == 0 {
                    return error("server closed the connection without a response")
                }
                return error("unexpected end of stream in header")
            }
            end = rtHeadEnd(b.buf, b.scan, b.w)
        }
        var h = map[string]string{}
        var parts = rtParseHead(b.buf, b.r, end, 1, h)?
        b.skip(end - b.r)
        var status = rtAtoi(parts[1])
        if status >= 100 && status < 200 && status != 101 {
            continue
        }
        return Response{Status: status, Headers: h, Proto: parts[0]}
    }
}

// ---- streamed bodies ------------------------------------------------------------

// BodyReader is a response body read from the connection (Client.Stream):
// an io.AsyncReader. Reading it to the end returns the connection to the
// pool; Close before the end closes the connection.
struct BodyReader {
    t  *Transport
    pc *pconn
    // 0 Content-Length, 1 chunked, 2 until the connection closes, 3 done
    mode    int
    left    int
    keep    bool
    gz      *std.compress.gzip.Reader
    pending []byte
    closed  bool
    // HTTP/2: the stream's data channel
    h2 *h2stream

    // Up to n bytes of the (decoded) body; empty at the end.
    func Read(n int) Task<[]byte!> {
        return bodyRead(this, n)
    }

    // The rest of the body.
    func ReadAll() Task<string!> {
        return bodyReadAll(this)
    }

    // Stops reading: the connection is closed unless the body was read
    // to its end.
    func Close() {
        if this.closed {
            return
        }
        this.closed = true
        if this.h2 != nil {
            h2CancelStream(this.h2)
            return
        }
        if this.mode != 3 && this.pc != nil {
            discardConn(this.t, this.pc)
            this.pc = nil
            this.mode = 3
        }
    }

    // A body dropped without Close or reading it to the end gives its
    // connection up (it cannot be reused).
    func Drop() {
        if this.closed {
            return
        }
        if this.h2 != nil {
            if this.h2.h != nil {
                h2forget(this.h2.h, this.h2)
            }
            return
        }
        if this.mode != 3 && this.pc != nil {
            discardConn(this.t, this.pc)
            this.pc = nil
        }
    }

    // A read failed: the connection cannot be reused.
    func broken() {
        this.mode = 3
        if this.pc != nil {
            discardConn(this.t, this.pc)
            this.pc = nil
        }
    }

    func finish() {
        if this.mode == 3 && this.pc == nil {
            return
        }
        this.mode = 3
        if this.pc != nil {
            putConn(this.t, this.pc, this.keep)
            this.pc = nil
        }
    }
}

// The next raw (still encoded) piece of the body; empty at the end.
async func bodyRaw(br *BodyReader, n int) []byte! {
    if br.h2 != nil {
        return await h2ReadBody(br.h2, n)?
    }
    if br.mode == 3 || br.pc == nil {
        return []
    }
    var c = br.pc.c
    var b = br.pc.b
    if br.mode == 0 {
        if br.left == 0 {
            br.finish()
            return []
        }
        if b.buffered() == 0 {
            if !await fill(c, b)? {
                discardConn(br.t, br.pc)
                br.pc = nil
                br.mode = 3
                return error("unexpected end of stream in body")
            }
        }
        var k = minInt(minInt(n, br.left), b.buffered())
        var out = std.bytes.Clone(b.buf[b.r..b.r + k])
        b.skip(k)
        br.left -= k
        if br.left == 0 {
            br.finish()
        }
        return out
    }
    if br.mode == 1 {
        if br.left == 0 {
            var line = await readLine(c, b, 4096)?
            var semi = std.strings.Index(line, ";")
            if semi >= 0 {
                line = line[..semi]
            }
            var size = parseHex(line)?
            if size == 0 {
                for {
                    var tr = await readLine(c, b, 65536)?
                    if tr == "" {
                        break
                    }
                }
                br.finish()
                return []
            }
            br.left = size
        }
        if b.buffered() == 0 {
            if !await fill(c, b)? {
                return error("unexpected end of stream in body")
            }
        }
        var k = minInt(minInt(n, br.left), b.buffered())
        var out = std.bytes.Clone(b.buf[b.r..b.r + k])
        b.skip(k)
        br.left -= k
        if br.left == 0 {
            await fillTo(c, b, 2)?
            b.skip(2)
        }
        return out
    }
    // until the end of the stream
    if b.buffered() == 0 {
        if !await fill(c, b)? {
            br.keep = false
            br.finish()
            return []
        }
    }
    var k = minInt(n, b.buffered())
    var out = std.bytes.Clone(b.buf[b.r..b.r + k])
    b.skip(k)
    return out
}

async func bodyRead(br *BodyReader, n int) []byte! {
    if br.closed {
        return ErrBodyClosed
    }
    if br.gz == nil {
        var b = try await bodyRaw(br, n) catch err {
            br.broken()
            return err
        }
        return b
    }
    for {
        if len(br.pending) > 0 {
            var k = minInt(n, len(br.pending))
            var out = std.bytes.Clone(br.pending[..k])
            br.pending = br.pending[k..]
            return out
        }
        var raw = try await bodyRaw(br, 32768) catch err {
            br.broken()
            return err
        }
        if len(raw) == 0 {
            br.gz.Close()?
            return []
        }
        try br.gz.Feed(raw) catch err {
            br.broken()
            return err
        }
        br.pending = br.gz.Read()
    }
}

async func bodyReadAll(br *BodyReader) string! {
    if br.gz == nil && br.h2 == nil && br.mode == 0 && br.pc != nil {
        // one buffer for the whole Content-Length body
        try await fillTo(br.pc.c, br.pc.b, br.left) catch err {
            br.broken()
            return err
        }
        var s = br.pc.b.take(br.left)
        br.left = 0
        br.finish()
        return s
    }
    var out = ""
    for {
        var chunk = await bodyRead(br, 65536)?
        if len(chunk) == 0 {
            return out
        }
        out += std.strings.FromBytes(chunk)
    }
}

// ---- dialing for other protocols (WebSockets) ------------------------------------

// Opens a raw connection to u (http/ws: TCP, https/wss: TLS with ALPN
// http/1.1) for protocols that start with an HTTP/1.1 upgrade.
async func DialRaw(u URL, cfg *std.net.tls.Config, timeoutMs int) RawConn! {
    var ms = timeoutMs
    if ms <= 0 {
        ms = 30000
    }
    var raw = await std.net.DialTimeout(u.Addr(), ms)?
    if u.Scheme == "https" || u.Scheme == "wss" {
        var conf = std.net.tls.Config{}
        if cfg != nil {
            conf = *cfg
        }
        if conf.ServerName == "" {
            conf.ServerName = u.Hostname
        }
        conf.ALPN = ["http/1.1"]
        var tc = try await std.net.tls.Client(raw, conf) catch err {
            raw.Close()
            return err
        }
        return RawConn{c: tlsConn(tc), b: newRbuf(4096)}
    }
    return RawConn{c: plainConn(raw), b: newRbuf(4096)}
}

// ---- cookies ---------------------------------------------------------------------

// CookieJar stores cookies from responses and sends them back (RFC 6265:
// host-only and domain cookies, paths, expiry, Secure). Safe for
// concurrent use.
struct CookieJar {
    guard   int
    entries []jarEntry

    // Stores the cookies of a response from u.
    func SetCookies(u URL, cookies []Cookie) {
        var now = std.time.Now() / 1000
        std.task.rtLock(&this.guard)
        for c in cookies {
            var e = jarEntry{c: c, hostOnly: c.Domain == ""}
            if e.hostOnly {
                e.c.Domain = std.strings.ToLower(u.Hostname)
            } else if !domainMatch(std.strings.ToLower(u.Hostname), e.c.Domain) {
                continue
            }
            if e.c.Path == "" || !std.strings.HasPrefix(e.c.Path, "/") {
                e.c.Path = defaultPath(u.Path)
            }
            var keep []jarEntry = []
            for x in this.entries {
                if !(x.c.Name == e.c.Name && x.c.Domain == e.c.Domain && x.c.Path == e.c.Path) {
                    keep = append(keep, x)
                }
            }
            var expired = c.MaxAge < 0 || (c.Expires > 0 && c.Expires <= now)
            if !expired {
                keep = append(keep, e)
            }
            this.entries = keep
        }
        std.task.rtUnlock(&this.guard)
    }

    // The cookies to send to u (longest paths first).
    func Cookies(u URL) []Cookie {
        var now = std.time.Now() / 1000
        var host = std.strings.ToLower(u.Hostname)
        var out []Cookie = []
        std.task.rtLock(&this.guard)
        var live []jarEntry = []
        for e in this.entries {
            if e.c.Expires > 0 && e.c.Expires <= now {
                continue
            }
            live = append(live, e)
            var hostOK = host == e.c.Domain || (!e.hostOnly && domainMatch(host, e.c.Domain))
            if !hostOK || !pathMatch(u.Path, e.c.Path) {
                continue
            }
            if e.c.Secure && u.Scheme != "https" && u.Scheme != "wss" {
                continue
            }
            out = append(out, e.c)
        }
        this.entries = live
        std.task.rtUnlock(&this.guard)
        // longest path first (insertion order otherwise)
        var i = 1
        for i < len(out) {
            var j = i
            for j > 0 && len(out[j].Path) > len(out[j - 1].Path) {
                var tmp = out[j]
                out[j] = out[j - 1]
                out[j - 1] = tmp
                j -= 1
            }
            i += 1
        }
        return out
    }

    // Number of stored cookies.
    func Len() int {
        std.task.rtLock(&this.guard)
        var n = len(this.entries)
        std.task.rtUnlock(&this.guard)
        return n
    }
}

struct jarEntry {
    c        Cookie
    hostOnly bool
}

// An empty cookie jar.
func NewCookieJar() *CookieJar {
    return new CookieJar{}
}

func domainMatch(host string, domain string) bool {
    if host == domain {
        return true
    }
    return std.strings.HasSuffix(host, "." + domain) && !std.net.rtIsIP(host)
}

func pathMatch(reqPath string, cookiePath string) bool {
    if reqPath == cookiePath {
        return true
    }
    if std.strings.HasPrefix(reqPath, cookiePath) {
        return std.strings.HasSuffix(cookiePath, "/") || reqPath[len(cookiePath)] == 47
    }
    return false
}

func defaultPath(p string) string {
    if p == "" || p[0] != 47 {
        return "/"
    }
    var i = std.strings.LastIndex(p, "/")
    if i == 0 {
        return "/"
    }
    return p[..i]
}
