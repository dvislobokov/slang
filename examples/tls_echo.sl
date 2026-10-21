namespace examples.tls_echo

import std
import std.crypto
import std.io
import std.net

// A TLS echo server and its clients in one program (std.net.tls): TLS 1.3
// and TLS 1.2, ALPN, a large message spread over many records, KeyUpdate,
// client certificates, session resumption, certificate verification
// against a pinned self-signed certificate, and HTTPS through std.net.http.
// The certificates are self-signed test certificates (valid 2000-2100)
// for localhost; never use embedded keys in real programs.

const serverCert = `-----BEGIN CERTIFICATE-----
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

const serverKey = `-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgjA2gD248y45QVG96
fXrFkd1iVNl1OOz2VN/OkZMN5jehRANCAASc1zlpmSuVZO3wBCodf/2ckVINRHTU
mOSXBzpVELQe6p45BK0zK1pyWibOTfHSGzqN94wNd2hMhWaKQIs0wMaQ
-----END PRIVATE KEY-----`

const clientCert = `-----BEGIN CERTIFICATE-----
MIIBZzCCARmgAwIBAgIBAjAFBgMrZXAwMTEXMBUGA1UEAwwOZXhhbXBsZSBjbGll
bnQxFjAUBgNVBAoMDVNsYW5nIGV4YW1wbGUwIBcNMDAwMTAxMDAwMDAwWhgPMjEw
MDAxMDEwMDAwMDBaMDExFzAVBgNVBAMMDmV4YW1wbGUgY2xpZW50MRYwFAYDVQQK
DA1TbGFuZyBleGFtcGxlMCowBQYDK2VwAyEAd8TTWfWJ2mX8ykx4z99zktc4t9jG
+V1xj6RRaypHP4+jVDBSMBMGA1UdJQQMMAoGCCsGAQUFBwMCMAwGA1UdEwEB/wQC
MAAwDgYDVR0PAQH/BAQDAgeAMB0GA1UdDgQWBBScbQie9+OO7P27IZxVU/ukM6tf
NDAFBgMrZXADQQDVvcFQ35Up8dPbyJjDt/+tNZvjEHIgA1eHRTjX5/R3MMNMPd7u
xLrjsqArSYFpGzC96w++ZfIoE1Dcn25dw3wP
-----END CERTIFICATE-----`

const clientKey = `-----BEGIN PRIVATE KEY-----
MC4CAQAwBQYDK2VwBCIEIP1OdHDvf/CEQ4+b2XmZRuENl//KA/mVMHqSz1VFs3ui
-----END PRIVATE KEY-----`

// Echoes lines; "who" answers with the client certificate's name.
async func echo(c tls.Conn) void! {
    for {
        var b = await c.Read(65536)?
        if len(b) == 0 {
            break
        }
        if strings.FromBytes(b) == "who" {
            var peers = c.ConnectionState().PeerCertificates
            var name = "anonymous"
            if len(peers) > 0 {
                name = peers[0].Subject.CommonName
            }
            _ = await c.WriteString(name)?
        } else {
            _ = await c.Write(b)?
        }
    }
    c.Close()
}

async func serve(l tls.Listener) {
    for {
        var c = try await l.Accept() catch err {
            return
        }
        _ = echo(c)
    }
}

// Writes msg and reads until as many bytes came back.
async func roundTrip(c tls.Conn, msg string) string! {
    _ = await c.WriteString(msg)?
    var got = ""
    for len(got) < len(msg) {
        var b = await c.ReadString(65536)?
        if b == "" {
            return error("connection closed early")
        }
        got += b
    }
    return got
}

async func main() void! {
    var srvCert = tls.LoadKeyPair(serverCert, serverKey)?
    var cliCert = tls.LoadKeyPair(clientCert, clientKey)?
    var clientCAs = x509.NewCertPool()
    clientCAs.AddCert(cliCert.Chain[0])
    var l = tls.Listen("127.0.0.1:0", tls.ServerConfig{
        Certificates: [srvCert],
        ALPN: ["echo/1"],
        ClientAuth: tls.VerifyClientCertIfGiven,
        ClientCAs: clientCAs,
    })?
    var addr = fmt.Format("127.0.0.1:{0}", l.Port())
    var serving = serve(l)

    // 1. TLS 1.3. InsecureSkipVerify accepts any certificate: fine for
    // this local self-signed test server, never for real servers.
    var a = await tls.Dial(addr, tls.Config{InsecureSkipVerify: true, ALPN: ["echo/1"]})?
    var st = a.ConnectionState()
    println(
        "version:",
        st.VersionName(),
        "alpn:",
        st.ALPN,
        "peer:",
        st.PeerCertificates[0].Subject.CommonName,
    )
    println("echo:", await roundTrip(a, "hello over TLS")?)
    var big = strings.Repeat("0123456789", 10000)
    println("big echo ok:", await roundTrip(a, big)? == big)
    await a.KeyUpdate()?
    println("after KeyUpdate:", await roundTrip(a, "still talking")?)
    println("who:", await roundTrip(a, "who")?)
    a.Close()

    // 2. TLS 1.2 with the server certificate pinned as the only root, a
    // client certificate and a session cache (the second dial resumes).
    var roots = x509.NewCertPool()
    roots.AddCert(srvCert.Chain[0])
    var cache = tls.NewSessionCache()
    var cfg = tls.Config{
        ServerName: "localhost",
        RootCAs: roots,
        MaxVersion: tls.V12,
        ClientCert: &cliCert,
        Sessions: cache,
    }
    for i in 0..2 {
        var b = await tls.Dial(addr, cfg)?
        var bs = b.ConnectionState()
        println(
            "dial",
            i + 1,
            bs.VersionName(),
            "verified:",
            len(bs.VerifiedChain),
            "resumed:",
            bs.Resumed,
            "who:",
            await roundTrip(b, "who")?,
        )
        b.Close()
    }

    // 3. TLS 1.3 resumption with a PSK from a session ticket.
    var c13 = tls.Config{ServerName: "localhost", RootCAs: roots, Sessions: tls.NewSessionCache()}
    for i in 0..2 {
        var c = await tls.Dial(addr, c13)?
        println(
            "dial",
            i + 1,
            c.ConnectionState().VersionName(),
            "resumed:",
            c.ConnectionState().Resumed,
            await roundTrip(c, "ok")?,
        )
        c.Close()
    }

    // 4. Verification failures.
    try await tls.Dial(addr, tls.Config{ServerName: "example.com", RootCAs: roots}) catch err {
        println("wrong name:", err)
    }
    try await tls.Dial(addr, tls.Config{RootCAs: x509.NewCertPool()}) catch err {
        println("unknown root:", err)
    }

    l.Close()
    await serving

    // 5. HTTPS: http.Server{TLS} and http.Client{TLS}.
    var r = http.NewRouter()
    r.Get("/hello", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, "hello over https\n")
    })
    var scfg = tls.ServerConfig{Certificates: [srvCert]}
    var srv = http.Server{Addr: "127.0.0.1:0", Handler: r, TLS: &scfg}
    srv.Listen()?
    var web = srv.Serve()
    var ccfg = tls.Config{RootCAs: roots}
    var client = http.Client{TLS: &ccfg}
    var res = await client.Get(fmt.Format("https://localhost:{0}/hello", srv.Port()))?
    print(res.Status, " ", res.Body)
    await srv.Shutdown()
    await web?
    println("done")
}
