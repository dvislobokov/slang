namespace std.net.http

// Middleware (Recover, Logging, CORS, Gzip, BodyLimit, TimeoutHandler),
// static files (ServeFile, ServeDir: ranges, ETag / Last-Modified,
// sendfile) and Server-Sent Events (ServerSentEvents writer, EventSource
// client). See http.sl for the file layout.

// ---- middleware --------------------------------------------------------------

// Recover turns a handler error into a logged 500 "Internal Server Error"
// without the error text (the server's default 500 includes it).
// ErrAbortHandler passes through. Panics are not recoverable in Slang:
// they still end the process.
func Recover() func(Handler) Handler {
    return func(next Handler) Handler {
        return recoverHandler{next: next}
    }
}

struct recoverHandler {
    next Handler

    func Serve(req *Request, res *Response) Task<void!> {
        return recoverServe(this.next, req, res)
    }
}

async func recoverServe(next Handler, req *Request, res *Response) void! {
    try await next.Serve(req, res) catch err {
        if std.errors.Is(err, ErrAbortHandler) {
            return err
        }
        std.log.Error(err, "HTTP {RequestMethod} {RequestPath} failed: {ErrorMessage}", req.Method, req.Path, err.Message())
        if res.Started() {
            return ErrAbortHandler
        }
        res.Headers = nil
        res.Text(500, "500 Internal Server Error\n")
    }
}

// Logging writes one event per request to logger (nil: std.log.Global()):
// "HTTP {RequestMethod} {RequestPath} responded {StatusCode} in {Elapsed} ms"
// at Information (Warning for 5xx), like Serilog's request logging.
func Logging(logger std.log.Logger) func(Handler) Handler {
    return func(next Handler) Handler {
        return logHandler{next: next, l: logger}
    }
}

struct logHandler {
    next Handler
    l std.log.Logger

    func Serve(req *Request, res *Response) Task<void!> {
        return logServe(this.next, this.l, req, res)
    }
}

async func logServe(next Handler, l std.log.Logger, req *Request, res *Response) void! {
    var t0 = std.time.NowNanos()
    var failure error
    try await next.Serve(req, res) catch err {
        failure = err
    }
    var status = res.Status
    if status == 0 {
        status = 200
    }
    if failure != nil {
        status = 500
    }
    var ms = float((std.time.NowNanos() - t0) / 1000) / 1000.0
    var lg = l
    if lg == nil {
        lg = std.log.Global()
    }
    if failure != nil {
        lg.Error(failure, "HTTP {RequestMethod} {RequestPath} responded {StatusCode} in {Elapsed} ms", req.Method, req.Path, status, ms)
    } else if status >= 500 {
        lg.Error("HTTP {RequestMethod} {RequestPath} responded {StatusCode} in {Elapsed} ms", req.Method, req.Path, status, ms)
    } else {
        lg.Information("HTTP {RequestMethod} {RequestPath} responded {StatusCode} in {Elapsed} ms", req.Method, req.Path, status, ms)
    }
    if failure != nil {
        return failure
    }
}

// CORS options (CORS middleware). Empty AllowedOrigins allows every origin.
struct CORSOptions {
    AllowedOrigins []string
    // default GET, HEAD, POST, PUT, PATCH, DELETE
    AllowedMethods []string
    // default: whatever the preflight asks for
    AllowedHeaders []string
    ExposedHeaders []string
    AllowCredentials bool
    // seconds a preflight may be cached (0: not sent)
    MaxAge int
}

// CORS answers preflight requests (OPTIONS with
// Access-Control-Request-Method: 204, not passed on) and adds
// Access-Control-Allow-Origin to responses for allowed origins.
func CORS(opts CORSOptions) func(Handler) Handler {
    return func(next Handler) Handler {
        return corsHandler{next: next, o: opts}
    }
}

struct corsHandler {
    next Handler
    o CORSOptions

    func Serve(req *Request, res *Response) Task<void!> {
        return corsServe(this.next, this.o, req, res)
    }
}

func originAllowed(o CORSOptions, origin string) bool {
    if len(o.AllowedOrigins) == 0 {
        return true
    }
    for a in o.AllowedOrigins {
        if a == "*" || a == origin {
            return true
        }
    }
    return false
}

async func corsServe(next Handler, o CORSOptions, req *Request, res *Response) void! {
    var origin = req.Headers["Origin"]
    if origin == "" {
        await next.Serve(req, res)?
        return
    }
    var allowed = originAllowed(o, origin)
    var value = origin
    if len(o.AllowedOrigins) == 0 && !o.AllowCredentials {
        value = "*"
    }
    var preflight = req.Method == "OPTIONS" && req.Headers["Access-Control-Request-Method"] != ""
    if preflight {
        res.Status = 204
        if allowed {
            res.SetHeader("Access-Control-Allow-Origin", value)
            var methods = o.AllowedMethods
            if len(methods) == 0 {
                methods = ["GET", "HEAD", "POST", "PUT", "PATCH", "DELETE"]
            }
            res.SetHeader("Access-Control-Allow-Methods", std.strings.Join(methods, ", "))
            if len(o.AllowedHeaders) > 0 {
                res.SetHeader("Access-Control-Allow-Headers", std.strings.Join(o.AllowedHeaders, ", "))
            } else if req.Headers["Access-Control-Request-Headers"] != "" {
                res.SetHeader("Access-Control-Allow-Headers", req.Headers["Access-Control-Request-Headers"])
            }
            if o.AllowCredentials {
                res.SetHeader("Access-Control-Allow-Credentials", "true")
            }
            if o.MaxAge > 0 {
                res.SetHeader("Access-Control-Max-Age", itoa(o.MaxAge))
            }
        }
        res.AddHeader("Vary", "Origin")
        return
    }
    if allowed {
        res.SetHeader("Access-Control-Allow-Origin", value)
        if o.AllowCredentials {
            res.SetHeader("Access-Control-Allow-Credentials", "true")
        }
        if len(o.ExposedHeaders) > 0 {
            res.SetHeader("Access-Control-Expose-Headers", std.strings.Join(o.ExposedHeaders, ", "))
        }
        if value != "*" {
            res.AddHeader("Vary", "Origin")
        }
    }
    await next.Serve(req, res)?
}

// Gzip compresses buffered response bodies of at least 1 KB with a
// compressible Content-Type (text/*, JSON, JavaScript, XML, SVG) when the
// client accepts gzip; streamed (Flush) and file bodies are sent as they
// are. level: 1..9 (0: 6).
func Gzip(level int) func(Handler) Handler {
    var lv = level
    if lv <= 0 {
        lv = 6
    }
    return func(next Handler) Handler {
        return gzipHandler{next: next, level: lv}
    }
}

struct gzipHandler {
    next Handler
    level int

    func Serve(req *Request, res *Response) Task<void!> {
        return gzipServe(this.next, this.level, req, res)
    }
}

func compressible(ct string) bool {
    var t = std.strings.ToLower(ct)
    return std.strings.HasPrefix(t, "text/") || std.strings.Contains(t, "json") || std.strings.Contains(t, "javascript") || std.strings.Contains(t, "xml") || std.strings.Contains(t, "svg")
}

async func gzipServe(next Handler, level int, req *Request, res *Response) void! {
    await next.Serve(req, res)?
    res.AddHeader("Vary", "Accept-Encoding")
    if !rtHasToken(req.Headers["Accept-Encoding"], "gzip") || res.Started() || res.file != "" || len(res.Body) < 1024 {
        return
    }
    if res.Header("Content-Encoding") != "" {
        return
    }
    var ct = res.Header("Content-Type")
    if ct == "" {
        ct = "text/plain"
    }
    if !compressible(ct) {
        return
    }
    var z = std.compress.gzip.Compress(std.strings.Bytes(res.Body), level)
    res.Body = std.strings.FromBytes(z)
    res.SetHeader("Content-Encoding", "gzip")
    if res.Header("Etag") != "" {
        // a different representation: weaken the validator
        var e = res.Header("Etag")
        if !std.strings.HasPrefix(e, "W/") {
            res.SetHeader("Etag", "W/" + e)
        }
    }
}

// BodyLimit answers 413 for request bodies over n bytes (the body has been
// read already; Server.MaxBodyBytes stops reading at the limit).
func BodyLimit(n int) func(Handler) Handler {
    return func(next Handler) Handler {
        return limitHandler{next: next, n: n}
    }
}

struct limitHandler {
    next Handler
    n int

    func Serve(req *Request, res *Response) Task<void!> {
        if len(req.Body) > this.n {
            return plainStatus(res, 413)
        }
        return this.next.Serve(req, res)
    }
}

// TimeoutHandler runs h with a time limit of ms milliseconds (its waits are
// cancelled through the task context when it expires) and answers 503
// with msg if it has not finished by then. The handler writes into a
// buffered response that is copied out when it finishes in time (Flush is
// buffered too).
func TimeoutHandler(h Handler, ms int, msg string) Handler {
    return timeoutHandler{next: h, ms: ms, msg: msg}
}

struct timeoutHandler {
    next Handler
    ms int
    msg string

    func Serve(req *Request, res *Response) Task<void!> {
        return timeoutServe(this.next, this.ms, this.msg, req, res)
    }
}

async func timeoutServe(next Handler, ms int, msg string, req *Request, res *Response) void! {
    var inner = new Response{Status: 200}
    var t0 = std.time.Now()
    var t = std.task.WithTimeout(ms, func() Task<void!> {
        return next.Serve(req, inner)
    })
    // a handler woken by the cancellation finishes at the deadline too
    if !await std.task.Timeout(t, ms) || std.time.Now() - t0 >= ms {
        res.Text(503, msg)
        return
    }
    try await t catch err {
        if std.errors.Is(err, std.task.ErrCancelled) {
            res.Text(503, msg)
            return
        }
        return err
    }
    res.Status = inner.Status
    res.Headers = inner.Headers
    res.Body = inner.Body
    res.file = inner.file
    res.fileOff = inner.fileOff
    res.fileLen = inner.fileLen
}

// ---- static files ------------------------------------------------------------------

// Resolves "." and ".." segments of a slash path (Go's path.Clean, always
// rooted).
func cleanURLPath(p string) string {
    var out []string = []
    for seg in std.strings.Split(p, "/") {
        if seg == "" || seg == "." {
            continue
        }
        if seg == ".." {
            if len(out) > 0 {
                out = out[..len(out) - 1]
            }
            continue
        }
        out = append(out, seg)
    }
    return "/" + std.strings.Join(out, "/")
}

func etagOf(size int, mtimeNanos int) string {
    return "\"" + hexString(mtimeNanos / 1000) + "-" + hexString(size) + "\""
}

func etagMatch(list string, etag string, weak bool) bool {
    var t = std.strings.Trim(list)
    if t == "*" {
        return true
    }
    var plain = std.strings.TrimPrefix(etag, "W/")
    for part in std.strings.Split(t, ",") {
        var p = std.strings.Trim(part)
        if p == etag {
            return true
        }
        if weak && std.strings.TrimPrefix(p, "W/") == plain {
            return true
        }
    }
    return false
}

struct byteRange {
    start int
    length int
}

// Parses a Range header against size: the ranges (nil for none/ignored),
// or an error for an unsatisfiable range.
func parseRanges(h string, size int) []byteRange! {
    if !std.strings.HasPrefix(h, "bytes=") {
        return nil
    }
    var out []byteRange = []
    for part in std.strings.Split(h[6..], ",") {
        var p = std.strings.Trim(part)
        if p == "" {
            continue
        }
        var dash = std.strings.Index(p, "-")
        if dash < 0 {
            return nil
        }
        var a = std.strings.Trim(p[..dash])
        var b = std.strings.Trim(p[dash + 1..])
        if a == "" {
            // the last b bytes
            var n = rtAtoi(b)
            if n < 0 {
                return nil
            }
            if n == 0 {
                continue
            }
            if n > size {
                n = size
            }
            out = append(out, byteRange{start: size - n, length: n})
            continue
        }
        var s = rtAtoi(a)
        if s < 0 {
            return nil
        }
        if s >= size {
            continue
        }
        var e = size - 1
        if b != "" {
            e = rtAtoi(b)
            if e < 0 || e < s {
                return nil
            }
            if e >= size {
                e = size - 1
            }
        }
        out = append(out, byteRange{start: s, length: e - s + 1})
    }
    if len(out) == 0 {
        return error("range not satisfiable")
    }
    return out
}

// ServeFile answers req with the file at path: Content-Type from the
// extension (std.mime), ETag and Last-Modified with conditional requests
// (If-None-Match, If-Modified-Since -> 304; If-Match,
// If-Unmodified-Since -> 412), byte ranges (206, multipart/byteranges for
// several, 416 when unsatisfiable, If-Range), HEAD, and the body sent with
// sendfile(2). A directory serves its index.html. Missing files get 404.
async func ServeFile(req *Request, res *Response, path string) void! {
    var info = try std.os.Stat(path) catch err {
        await plainStatus(res, 404)?
        return
    }
    if info.IsDir {
        var index = std.fs.Join(path, "index.html")
        info = try std.os.Stat(index) catch err {
            await plainStatus(res, 403)?
            return
        }
        if info.IsDir {
            await plainStatus(res, 403)?
            return
        }
        await serveRegular(req, res, index, info)?
        return
    }
    await serveRegular(req, res, path, info)?
}

async func serveRegular(req *Request, res *Response, path string, info std.os.FileInfo) void! {
    var size = info.Size
    var mtime = info.ModTimeNanos / 1000000000
    var etag = etagOf(size, info.ModTimeNanos)
    var lastMod = rtHttpTime(mtime)
    res.SetHeader("Accept-Ranges", "bytes")
    res.SetHeader("Etag", etag)
    res.SetHeader("Last-Modified", lastMod)
    // preconditions (RFC 9110 13.2.2)
    var im = req.Headers["If-Match"]
    if im != "" && !etagMatch(im, etag, false) {
        res.Text(412, "412 Precondition Failed\n")
        return
    }
    var ius = req.Headers["If-Unmodified-Since"]
    if im == "" && ius != "" {
        var t = rtParseHttpTime(ius)
        if t >= 0 && mtime > t {
            res.Text(412, "412 Precondition Failed\n")
            return
        }
    }
    var inm = req.Headers["If-None-Match"]
    if inm != "" {
        if etagMatch(inm, etag, true) && (req.Method == "GET" || req.Method == "HEAD") {
            res.Status = 304
            return
        }
    } else {
        var ims = req.Headers["If-Modified-Since"]
        if ims != "" && (req.Method == "GET" || req.Method == "HEAD") {
            var t = rtParseHttpTime(ims)
            if t >= 0 && mtime <= t {
                res.Status = 304
                return
            }
        }
    }
    var ctype = res.Header("Content-Type")
    if ctype == "" {
        ctype = std.mime.TypeByExtension(std.fs.Ext(path))
        if ctype == "" {
            ctype = "application/octet-stream"
        }
        res.SetHeader("Content-Type", ctype)
    }
    var rh = req.Headers["Range"]
    var ir = req.Headers["If-Range"]
    if rh != "" && ir != "" {
        // the range applies only if the representation is unchanged
        var same = ir == etag
        if !same && !std.strings.HasPrefix(ir, "\"") && !std.strings.HasPrefix(ir, "W/") {
            var t = rtParseHttpTime(ir)
            same = t >= 0 && t == mtime
        }
        if !same {
            rh = ""
        }
    }
    if rh == "" || req.Method != "GET" && req.Method != "HEAD" {
        res.Status = 200
        res.SendFile(path, 0, size)?
        return
    }
    var ranges = try parseRanges(rh, size) catch err {
        res.SetHeader("Content-Range", "bytes */" + itoa(size))
        res.Text(416, "416 Range Not Satisfiable\n")
        return
    }
    if ranges == nil {
        res.Status = 200
        res.SendFile(path, 0, size)?
        return
    }
    if len(ranges) == 1 {
        var r = ranges[0]
        res.Status = 206
        res.SetHeader("Content-Range", "bytes " + itoa(r.start) + "-" + itoa(r.start + r.length - 1) + "/" + itoa(size))
        res.SendFile(path, r.start, r.length)?
        return
    }
    // several ranges: multipart/byteranges, built in memory
    var f = std.os.Open(path)?
    var boundary = "slang" + hexString(std.random.IntN(1 << 30)) + hexString(size)
    var body = std.strings.Builder{}
    for r in ranges {
        var buf = std.net.MakeBuffer(r.length)
        var k = try f.ReadAt(buf, r.start) catch err {
            _ = f.Close()
            return err
        }
        body.Write("\r\n--" + boundary + "\r\nContent-Type: " + ctype + "\r\nContent-Range: bytes " + itoa(r.start) + "-" + itoa(r.start + r.length - 1) + "/" + itoa(size) + "\r\n\r\n")
        body.Write(std.strings.FromBytes(buf[..k]))
    }
    _ = f.Close()
    body.Write("\r\n--" + boundary + "--\r\n")
    res.Status = 206
    res.SetHeader("Content-Type", "multipart/byteranges; boundary=" + boundary)
    res.Body = body.String()
}

// ServeDir serves the files under root: the request path (the "*"
// parameter of a Router route like "/static/*", else req.Path) is cleaned
// (".." cannot leave root) and joined to root. Directories redirect to
// their slash form, serve index.html, or list their entries (Listing).
struct FileServer {
    Root string
    // HTML listings of directories without index.html (else 403).
    Listing bool

    func Serve(req *Request, res *Response) Task<void!> {
        return serveDir(this.Root, this.Listing, req, res)
    }
}

// A FileServer for root with directory listings.
func ServeDir(root string) Handler {
    return FileServer{Root: root, Listing: true}
}

async func serveDir(root string, listing bool, req *Request, res *Response) void! {
    if req.Method != "GET" && req.Method != "HEAD" {
        res.SetHeader("Allow", "GET, HEAD")
        await plainStatus(res, 405)?
        return
    }
    var rel = req.Path
    if req.Params != nil {
        var p = req.Params["*"]
        if p != "" || std.strings.HasSuffix(req.Path, "/") {
            rel = "/" + p
        }
    }
    var clean = cleanURLPath(unescape(std.strings.Replace(rel, "+", "%2B")))
    var full = root + clean
    var info = try std.os.Stat(full) catch err {
        await plainStatus(res, 404)?
        return
    }
    if info.IsDir {
        if !std.strings.HasSuffix(req.Path, "/") {
            var loc = req.Path + "/"
            if req.Query != "" {
                loc += "?" + req.Query
            }
            res.Redirect(301, loc)
            return
        }
        var index = std.fs.Join(full, "index.html")
        var ii = try std.os.Stat(index) else std.os.FileInfo{IsDir: true}
        if !ii.IsDir {
            await serveRegular(req, res, index, ii)?
            return
        }
        if !listing {
            await plainStatus(res, 403)?
            return
        }
        var names []string = []
        for e in std.os.ReadDir(full)? {
            if e.IsDir {
                names = append(names, e.Name + "/")
            } else {
                names = append(names, e.Name)
            }
        }
        var b = std.strings.Builder{}
        b.Write("<!doctype html>\n<meta name=\"viewport\" content=\"width=device-width\">\n<title>" + std.html.Escape(clean) + "</title>\n<pre>\n")
        for n in names {
            b.Write("<a href=\"" + std.html.Escape(n) + "\">" + std.html.Escape(n) + "</a>\n")
        }
        b.Write("</pre>\n")
        res.Html(200, b.String())
        return
    }
    await serveRegular(req, res, full, info)?
}

// ---- Server-Sent Events ----------------------------------------------------------

// An event of a text/event-stream (HTML Living Standard 9.2).
struct SSEEvent {
    ID string
    // the event type ("" = "message")
    Event string
    Data string
    // reconnection time in ms (0: not sent)
    Retry int
}

// EventWriter sends Server-Sent Events on a response (ServerSentEvents).
struct EventWriter {
    res *Response

    // Sends a "message" event with data (several lines allowed).
    func Send(data string) Task<void!> {
        return this.SendEvent(SSEEvent{Data: data})
    }

    func SendEvent(ev SSEEvent) Task<void!> {
        this.res.Write(formatEvent(ev))
        return this.res.Flush()
    }

    // A comment line (": text"), e.g. as a keep-alive.
    func Comment(text string) Task<void!> {
        this.res.Write(": " + text + "\n\n")
        return this.res.Flush()
    }
}

func formatEvent(ev SSEEvent) string {
    var out = ""
    if ev.ID != "" {
        out += "id: " + ev.ID + "\n"
    }
    if ev.Event != "" {
        out += "event: " + ev.Event + "\n"
    }
    if ev.Retry > 0 {
        out += "retry: " + itoa(ev.Retry) + "\n"
    }
    for line in std.strings.Split(std.strings.Replace(ev.Data, "\r\n", "\n"), "\n") {
        out += "data: " + line + "\n"
    }
    return out + "\n"
}

// Starts an event stream on res (Content-Type text/event-stream,
// Cache-Control no-cache) and returns its writer; the handler then sends
// events until it returns.
func ServerSentEvents(res *Response) *EventWriter {
    res.Status = 200
    res.SetHeader("Content-Type", "text/event-stream")
    res.SetHeader("Cache-Control", "no-cache")
    res.SetHeader("X-Accel-Buffering", "no")
    return new EventWriter{res: res}
}

// EventSource reads Server-Sent Events from url: `for ev in es` (or
// `await es.Next()`) yields events; when the stream ends it reconnects
// after the retry time (default 3000 ms, or the server's `retry:`) with
// Last-Event-ID, up to MaxRetries times in a row (default 3, negative =
// never reconnect). Close stops it.
struct EventSource {
    URL string
    Client Client
    Headers map[string]string
    MaxRetries int
    // the last event id received (sent back as Last-Event-ID)
    LastEventID string
    retry int
    body *BodyReader
    buf string
    closed bool
    failures int

    async func Next() (SSEEvent, bool) {
        var ev = try await this.next() catch err {
            return (SSEEvent{}, false)
        }
        return (ev, !this.closed || ev.Data != "" || ev.Event != "")
    }

    // The next event; fails when the stream cannot be (re)opened.
    func Recv() Task<SSEEvent!> {
        return this.next()
    }

    func next() Task<SSEEvent!> {
        return esNext(this)
    }

    func Close() {
        this.closed = true
        if this.body != nil {
            this.body.Close()
            this.body = nil
        }
    }
}

// A new EventSource for url (default client).
func NewEventSource(url string) *EventSource {
    return new EventSource{URL: url, retry: 3000, MaxRetries: 3, Client: Client{}}
}

async func esConnect(es *EventSource) void! {
    var req = NewRequest("GET", es.URL).Header("Accept", "text/event-stream").Header("Cache-Control", "no-cache")
    for k, v in es.Headers {
        req.Header(k, v)
    }
    if es.LastEventID != "" {
        req.Header("Last-Event-ID", es.LastEventID)
    }
    var res = await es.Client.Stream(req)?
    if res.Status != 200 {
        if res.Reader != nil {
            res.Reader.Close()
        }
        es.MaxRetries = -1
        return error("EventSource " + es.URL + ": status " + itoa(res.Status))
    }
    if !std.strings.HasPrefix(res.Headers["Content-Type"], "text/event-stream") {
        if res.Reader != nil {
            res.Reader.Close()
        }
        es.MaxRetries = -1
        return error("EventSource " + es.URL + ": Content-Type " + res.Headers["Content-Type"])
    }
    es.body = res.Reader
    es.buf = ""
}

async func esNext(es *EventSource) SSEEvent! {
    var ev = SSEEvent{}
    var data []string = []
    var have = false
    for {
        if es.closed {
            return std.net.ErrClosed
        }
        if es.body == nil {
            if es.failures > 0 {
                if es.MaxRetries < 0 || es.failures > es.MaxRetries {
                    es.closed = true
                    return error("EventSource " + es.URL + ": stream ended")
                }
                await std.task.Delay(es.retry)
            }
            try await esConnect(es) catch err {
                es.failures += 1
                if es.MaxRetries < 0 || es.failures > es.MaxRetries {
                    es.closed = true
                    return err
                }
                continue
            }
        }
        var nl = std.strings.Index(es.buf, "\n")
        if nl < 0 {
            var chunk = try await es.body.Read(16384) catch err {
                es.body = nil
                es.failures += 1
                continue
            }
            if len(chunk) == 0 {
                es.body = nil
                es.failures += 1
                continue
            }
            es.buf += std.strings.FromBytes(chunk)
            continue
        }
        var line = es.buf[..nl]
        es.buf = es.buf[nl + 1..]
        if std.strings.HasSuffix(line, "\r") {
            line = line[..len(line) - 1]
        }
        if line == "" {
            if have {
                ev.Data = std.strings.Join(data, "\n")
                if ev.ID != "" {
                    es.LastEventID = ev.ID
                }
                es.failures = 0
                return ev
            }
            continue
        }
        if line[0] == 58 {
            continue
        }
        var field = line
        var value = ""
        var c = std.strings.Index(line, ":")
        if c >= 0 {
            field = line[..c]
            value = line[c + 1..]
            if std.strings.HasPrefix(value, " ") {
                value = value[1..]
            }
        }
        if field == "data" {
            data = append(data, value)
            have = true
        } else if field == "event" {
            ev.Event = value
            have = true
        } else if field == "id" {
            ev.ID = value
            have = true
        } else if field == "retry" {
            var n = rtAtoi(value)
            if n >= 0 {
                es.retry = n
                ev.Retry = n
            }
        }
    }
}
