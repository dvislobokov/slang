namespace std.net.http.test

// std.net.http.test (Go's net/http/httptest): a real server on a free
// loopback port (NewServer, NewTLSServer, NewH2CServer) with a client set
// up for it, a response recorder (NewRecorder) and test requests
// (NewRequest) to call a Handler directly (Serve).

// Server is a running test server. URL is its base ("http://127.0.0.1:port"
// or https); Client talks to it (TLS servers: the test certificate is
// trusted, ALPN h2; H2C servers: HTTP/2 with prior knowledge).
struct Server {
    URL string
    Client std.net.http.Client
    Transport *std.net.http.Transport
    srv *std.net.http.Server
    serving Task<void!>
    stopped bool

    func Port() int {
        return this.srv.Port()
    }

    // Stops the server (Shutdown) and waits for it; idle client
    // connections are closed first.
    func Close() Task<void> {
        return closeServer(this)
    }
}

async func closeServer(s *Server) {
    if s.stopped {
        return
    }
    s.stopped = true
    s.Transport.CloseIdleConnections()
    await s.srv.Shutdown()
    try await s.serving catch err {}
}

// Options of NewServerWith.
struct Options {
    // HTTPS with a self-signed certificate for localhost and 127.0.0.1.
    TLS bool
    // HTTP/2 with prior knowledge on plain TCP (server and client).
    H2C bool
    // The client of the server does not use HTTP/2 over TLS.
    DisableHTTP2 bool
    // Server.MaxBodyBytes, Server.IdleTimeout.
    MaxBodyBytes int
    IdleTimeout int
}

// A test server for h on 127.0.0.1 (a free port).
func NewServer(h std.net.http.Handler) *Server! {
    return NewServerWith(h, Options{})?
}

// A test HTTPS server (self-signed certificate, ALPN h2 and http/1.1).
func NewTLSServer(h std.net.http.Handler) *Server! {
    return NewServerWith(h, Options{TLS: true})?
}

// A test server speaking HTTP/2 with prior knowledge (and HTTP/1.1).
func NewH2CServer(h std.net.http.Handler) *Server! {
    return NewServerWith(h, Options{H2C: true})?
}

func NewServerWith(h std.net.http.Handler, o Options) *Server! {
    var srv = new std.net.http.Server{Addr: "127.0.0.1:0", Handler: h, H2C: o.H2C, MaxBodyBytes: o.MaxBodyBytes, IdleTimeout: o.IdleTimeout}
    var t = new std.net.http.Transport{H2C: o.H2C, DisableHTTP2: o.DisableHTTP2}
    if o.TLS {
        var pair = std.net.tls.LoadKeyPair(testCert(), testKey())?
        var scfg = std.net.tls.ServerConfig{Certificates: [pair]}
        srv.TLS = &scfg
        var ccfg = std.net.tls.Config{InsecureSkipVerify: true}
        t.TLS = &ccfg
    }
    srv.Listen()?
    var s = new Server{srv: srv, Transport: t}
    s.URL = srv.URL()
    s.Client = std.net.http.Client{Transport: t, Timeout: 30000}
    s.serving = srv.Serve()
    return s
}

// A response recorder: pass it to Handler.Serve and inspect Status,
// Headers and Body afterwards (Flush only counts: res.Flushes()).
func NewRecorder() *std.net.http.Response {
    return new std.net.http.Response{Status: 200}
}

// A server request for calling a Handler directly: target is a path
// ("/a?b=1") or a URL ("https://example.com/a"); Host defaults to
// example.com, RemoteAddr is 192.0.2.1:1234, a body sets Content-Length.
func NewRequest(method string, target string, body string) *std.net.http.Request {
    var req = new std.net.http.Request{Method: method, Proto: "HTTP/1.1", Headers: map[string]string{}, Body: body, RemoteAddr: "192.0.2.1:1234", Scheme: "http", Host: "example.com"}
    var path = target
    if std.strings.Contains(target, "://") {
        var u = try std.net.http.ParseURL(target) else std.net.http.URL{Path: "/"}
        req.Scheme = u.Scheme
        req.Host = u.Host
        path = u.RequestURI()
    }
    var q = std.strings.Index(path, "?")
    if q >= 0 {
        req.Query = path[q + 1..]
        path = path[..q]
    }
    if path == "" {
        path = "/"
    }
    req.Path = path
    req.Headers["Host"] = req.Host
    if body != "" {
        req.Headers["Content-Length"] = std.fmt.Format("{0}", len(body))
    }
    return req
}

// Serves req with h into a new recorder; a handler error becomes a 500
// response like the server's.
async func Serve(h std.net.http.Handler, req *std.net.http.Request) *std.net.http.Response {
    var rec = NewRecorder()
    try await h.Serve(req, rec) catch err {
        rec = NewRecorder()
        rec.Text(500, "500 Internal Server Error: " + err.Message() + "\n")
    }
    return rec
}

// The self-signed test certificate (localhost, 127.0.0.1; 2000-2100) and
// its P-256 key. For tests only.
func testCert() string {
    return "-----BEGIN CERTIFICATE-----\nMIIBuDCCAV+gAwIBAgIBATAKBggqhkjOPQQDAjAsMRIwEAYDVQQDDAlsb2NhbGhv\nc3QxFjAUBgNVBAoMDVNsYW5nIGV4YW1wbGUwIBcNMDAwMTAxMDAwMDAwWhgPMjEw\nMDAxMDEwMDAwMDBaMCwxEjAQBgNVBAMMCWxvY2FsaG9zdDEWMBQGA1UECgwNU2xh\nbmcgZXhhbXBsZTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABJzXOWmZK5Vk7fAE\nKh1//ZyRUg1EdNSY5JcHOlUQtB7qnjkErTMrWnJaJs5N8dIbOo33jA13aEyFZopA\nizTAxpCjcDBuMBoGA1UdEQQTMBGCCWxvY2FsaG9zdIcEfwAAATATBgNVHSUEDDAK\nBggrBgEFBQcDATAMBgNVHRMBAf8EAjAAMA4GA1UdDwEB/wQEAwIHgDAdBgNVHQ4E\nFgQU7c+7taSuwUvfHVxVltWQJ7fWbREwCgYIKoZIzj0EAwIDRwAwRAIgAq6gOamD\n/dF41P3SranwURDjPyw8ft+4ANNGannWFTcCIGKQWOvuMslR8TEtBRV+f1mHFRkU\nkW/WexisenH83QpJ\n-----END CERTIFICATE-----"
}

func testKey() string {
    return "-----BEGIN PRIVATE KEY-----\nMIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgjA2gD248y45QVG96\nfXrFkd1iVNl1OOz2VN/OkZMN5jehRANCAASc1zlpmSuVZO3wBCodf/2ckVINRHTU\nmOSXBzpVELQe6p45BK0zK1pyWibOTfHSGzqN94wNd2hMhWaKQIs0wMaQ\n-----END PRIVATE KEY-----"
}
