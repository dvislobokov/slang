namespace fixtures.h2.server

import std
import std.net

// The HTTP server of the interop tests in compiler/tests/net_http.rs: an
// h2c (prior knowledge) + HTTP/1.1 listener and an https listener (ALPN h2,
// http/1.1) with a self-signed localhost certificate, and a WebSocket echo
// endpoint /ws. Prints
// "h2c <port>" and "tls <port>" on stderr, then serves until "/quit".

const cert = `-----BEGIN CERTIFICATE-----
MIIBuDCCAV+gAwIBAgIBATAKBggqhkjOPQQDAjAsMRIwEAYDVQQDDAlsb2NhbGhv
c3QxFjAUBgNVBAoMDVNsYW5nIGV4YW1wbGUwIBcNMDAwMTAxMDAwMDAwWhgPMjEw
MDAxMDEwMDAwMDBaMCwxEjAQBgNVBAMMCWxvY2FsaG9zdDEWMBQGA1UECgwNU2xh
bmcgZXhhbXBsZTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABJzXOWmZK5Vk7fAE
Kh1//ZyRUg1EdNSY5JcHOlUQtB7qnjkErTMrWnJaJs5N8dIbOo33jA13aEyFZopA
izTAxpCjcDBuMBoGA1UdEQQTMBGCCWxvY2FsaG9zdIcEfwAAATATBgNVHSUEDDAK
BggrBgEFBQcDATAMBgNVHRMBAf8EAjAAMA4GA1UdDwEB/wQEAwIHgDAdBgNVHQ4E
FgQU7c+7taSuwUvfHVxVltWQJ7fWbREwCgYIKoZIzj0EAwIDRwAwRAIgAq6gOamD
/dF41P3SranwURDjPyw8ft+4ANNGannWFTcCIGKQWOvuMslR8TEtBRV+f1mHFRkU
kW/WexisenH83QpJ
-----END CERTIFICATE-----`

const key = `-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgjA2gD248y45QVG96
fXrFkd1iVNl1OOz2VN/OkZMN5jehRANCAASc1zlpmSuVZO3wBCodf/2ckVINRHTU
mOSXBzpVELQe6p45BK0zK1pyWibOTfHSGzqN94wNd2hMhWaKQIs0wMaQ
-----END PRIVATE KEY-----`

async func main() void! {
    var quit = task.Channel<int>(1)
    var r = http.NewRouter()
    r.Get("/hello", async func(req *http.Request, res *http.Response) void! {
        res.SetHeader("X-Proto", req.Proto)
        res.Text(200, "hello via " + req.Proto + "\n")
    })
    r.Post("/echo", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, fmt.Format("{0} bytes\n", len(req.Body)))
    })
    r.Get("/big", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, strings.Repeat("abcdefghij", 100000))
    })
    r.Get("/ws", async func(req *http.Request, res *http.Response) void! {
        var conn = try await ws.Upgrade(req, res) catch err {
            return
        }
        for msg in conn {
            await conn.Send(msg)?
        }
    })
    r.Get("/quit", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, "bye\n")
        await quit.Send(1)
    })
    var pair = tls.LoadKeyPair(cert, key)?
    var scfg = tls.ServerConfig{Certificates: [pair]}
    var plain = http.Server{Addr: "127.0.0.1:0", Handler: r, H2C: true}
    var secure = http.Server{Addr: "127.0.0.1:0", Handler: r, TLS: &scfg}
    plain.Listen()?
    secure.Listen()?
    var a = plain.Serve()
    var b = secure.Serve()
    io.Eprintln(fmt.Format("h2c {0}", plain.Port()))
    io.Eprintln(fmt.Format("tls {0}", secure.Port()))
    _ = await quit.Recv()
    await plain.Shutdown()
    await secure.Shutdown()
    await a?
    await b?
}
