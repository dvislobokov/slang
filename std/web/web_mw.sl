// Built-in middleware of App (each `UseX` adds one, in call order) and the
// health-check endpoint. Middleware of std.net.http (`http.Gzip`,
// `http.CORS`, `http.TimeoutHandler`) wraps the whole application through
// UseHttp; the others are web middleware that see the Context.
namespace std.web

// Options of UseRateLimit: a token bucket per client IP that refills
// Rate tokens per second up to Burst; a request takes one token or gets
// 429 with Retry-After.
struct RateLimitOptions {
    Rate float
    Burst int
    // Milliseconds clock (nil: std.time.Now), for tests.
    Clock func() int
}

struct bucket {
    tokens float
    last int
}

struct limiter {
    o RateLimitOptions
    buckets map[string]*bucket
    mu std.task.Mutex

    func now() int {
        if this.o.Clock != nil {
            return this.o.Clock()
        }
        return std.time.Now()
    }
}

// Options of UseRequestId.
struct RequestIdOptions {
    // default X-Request-Id
    Header string
    // nil: a random UUID
    Generate func() string
}

// Options of UseAuth (JWT bearer tokens).
struct AuthOptions {
    Key std.web.auth.jwt.Key
    Issuer string
    Audience string
    // Seconds of clock skew allowed for exp/nbf.
    Leeway int
    // Unix-seconds clock (nil: the system clock), for tests.
    Clock func() int
    // The claim holding the roles (default "roles").
    RolesClaim string
}

// TimeoutHandler runs h with a time limit (std.net.http.TimeoutHandler:
// 503 with msg when it expires); App.UseTimeout applies it to the app.
func TimeoutHandler(h std.net.http.Handler, ms int, msg string) std.net.http.Handler {
    return std.net.http.TimeoutHandler(h, ms, msg)
}

struct healthCheck {
    name string
    check func() Task<void!>
}

// The client address of a request (RemoteAddr without the port).
func clientIp(req *std.net.http.Request) string {
    var addr = req.RemoteAddr
    var colon = std.strings.LastIndex(addr, ":")
    if colon > 0 && !std.strings.HasSuffix(addr, "]") {
        addr = addr[..colon]
    }
    return std.strings.TrimSuffix(std.strings.TrimPrefix(addr, "["), "]")
}

async func logRequest(l std.log.Logger, c *Context, next *Next) void! {
    var t0 = std.time.NowNanos()
    var failure error
    try await next.Run(c) catch err {
        failure = err
    }
    var status = c.Response.Status
    if status == 0 {
        status = 200
    }
    if failure != nil {
        status = 500
    }
    var ms = float((std.time.NowNanos() - t0) / 1000) / 1000.0
    var route = c.Request.Path
    if c.Endpoint != nil {
        route = c.Endpoint.Pattern
    }
    if failure != nil {
        l.Error(failure, "HTTP {RequestMethod} {RequestPath} responded {StatusCode} in {Elapsed:0.0000} ms", c.Request.Method, c.Request.Path, status, ms)
        return failure
    }
    if status >= 500 {
        l.Error("HTTP {RequestMethod} {RequestPath} responded {StatusCode} in {Elapsed:0.0000} ms", c.Request.Method, c.Request.Path, status, ms)
    } else {
        l.ForContext("Route", route).Information("HTTP {RequestMethod} {RequestPath} responded {StatusCode} in {Elapsed:0.0000} ms", c.Request.Method, c.Request.Path, status, ms)
    }
}

async func recoverRequest(c *Context, next *Next) void! {
    try await next.Run(c) catch err {
        writeError(c, err)
    }
}

async func staticFiles(prefix string, dir string, c *Context, next *Next) void! {
    var req = c.Request
    if (req.Method == "GET" || req.Method == "HEAD") && (req.Path == prefix || std.strings.HasPrefix(req.Path, prefix + "/") || prefix == "/") {
        var rel = std.net.http.cleanURLPath(std.strings.TrimPrefix(req.Path, prefix))
        var path = dir + rel
        var info = try std.os.Stat(path) catch err {
            await next.Run(c)?
            return
        }
        if info.IsDir {
            path = path + "/index.html"
            var idx = try std.os.Stat(path) catch err {
                await next.Run(c)?
                return
            }
            _ = idx
        }
        await std.net.http.ServeFile(req, c.Response, path)?
        return
    }
    await next.Run(c)?
}

async func rateLimit(l *limiter, c *Context, next *Next) void! {
    var ip = clientIp(c.Request)
    var now = l.now()
    await l.mu.Lock()
    var b = l.buckets[ip]
    if b == nil {
        b = new bucket{tokens: float(l.o.Burst), last: now}
        l.buckets[ip] = b
    }
    var elapsed = float(now - b.last) / 1000.0
    b.last = now
    b.tokens = std.math.Min(float(l.o.Burst), b.tokens + elapsed * l.o.Rate)
    var allowed = b.tokens >= 1.0
    if allowed {
        b.tokens -= 1.0
    }
    var wait = 1
    if !allowed && l.o.Rate > 0.0 {
        wait = int(std.math.Ceil((1.0 - b.tokens) / l.o.Rate))
        if wait < 1 {
            wait = 1
        }
    }
    l.mu.Unlock()
    if !allowed {
        c.Response.SetHeader("Retry-After", std.fmt.Format("{0}", wait))
        writeProblem(c, problemFor(429, "rate limit exceeded; retry in " + std.fmt.Format("{0}", wait) + " s"))
        return
    }
    await next.Run(c)?
}

async func requestId(o RequestIdOptions, c *Context, next *Next) void! {
    var h = o.Header
    if h == "" {
        h = "X-Request-Id"
    }
    var id = c.Request.Header(h)
    if id == "" {
        if o.Generate != nil {
            id = o.Generate()
        } else {
            id = std.uuid.New()
        }
    }
    c.RequestId = id
    c.Response.SetHeader(h, id)
    var scope = std.log.Context.Push("RequestId", id)
    defer scope.Pop()
    await next.Run(c)?
}

async func traceRequest(c *Context, next *Next) void! {
    var span = std.diag.StartSpanKind("HTTP " + c.Request.Method, std.diag.SpanKind.Server)
    defer span.End()
    span.SetAttribute("http.request.method", c.Request.Method)
    span.SetAttribute("url.path", c.Request.Path)
    var failure error
    try await next.Run(c) catch err {
        failure = err
    }
    if c.Endpoint != nil {
        span.Name = "HTTP " + c.Request.Method + " " + c.Endpoint.Pattern
        span.SetAttribute("http.route", c.Endpoint.Pattern)
    }
    var status = c.Response.Status
    if status == 0 {
        status = 200
    }
    span.SetAttribute("http.response.status_code", status)
    if failure != nil {
        span.RecordError(failure)
        return failure
    }
    if status >= 500 {
        span.SetStatus(std.diag.StatusCode.Error, std.net.http.StatusText(status))
    }
}

async func authenticate(o AuthOptions, c *Context, next *Next) void! {
    var h = c.Request.Header("Authorization")
    if h != "" {
        if !std.strings.HasPrefix(h, "Bearer ") {
            c.Response.SetHeader("WWW-Authenticate", "Bearer")
            writeProblem(c, problemFor(401, "the Authorization header must be a Bearer token"))
            return
        }
        var token = std.strings.Trim(h[7..])
        var vo = std.web.auth.jwt.VerifyOptions{Issuer: o.Issuer, Audience: o.Audience, Leeway: o.Leeway, Clock: o.Clock}
        var claims = try std.web.auth.jwt.Verify(token, o.Key, vo) catch err {
            c.Response.SetHeader("WWW-Authenticate", "Bearer error=\"invalid_token\"")
            writeProblem(c, problemFor(401, err.Message()))
            return
        }
        var roles = claims.Roles
        if o.RolesClaim != "" && o.RolesClaim != "roles" {
            roles = claims.Strings(o.RolesClaim)
        }
        c.User = new Principal{Id: claims.Subject, Name: claims.Name, Roles: roles, Claims: claims}
    }
    await next.Run(c)?
}

async func bodyLimit(n int, c *Context, next *Next) void! {
    if len(c.Request.Body) > n {
        writeProblem(c, problemFor(413, "the request body is larger than " + std.fmt.Format("{0}", n) + " bytes"))
        return
    }
    await next.Run(c)?
}

async func runHealth(checks []healthCheck, c *Context) void! {
    var b = std.strings.Builder{}
    var healthy = true
    b.Write("{\"checks\":{")
    for i, h in checks {
        if i > 0 {
            b.Write(",")
        }
        b.Write(std.encoding.json.Quote(h.name))
        b.Write(":")
        try await h.check() catch err {
            healthy = false
            b.Write("{\"status\":\"Unhealthy\",\"description\":")
            b.Write(std.encoding.json.Quote(err.Message()))
            b.Write("}")
            continue
        }
        b.Write("{\"status\":\"Healthy\"}")
    }
    b.Write("},\"status\":")
    if healthy {
        b.Write("\"Healthy\"}")
        c.Response.Json(200, b.String())
    } else {
        b.Write("\"Unhealthy\"}")
        c.Response.Json(503, b.String())
    }
}
