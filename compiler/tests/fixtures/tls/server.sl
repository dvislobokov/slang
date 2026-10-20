namespace tlstest.server

import std.io
import std
import std.net
import std.crypto

// Interop server for compiler/tests/tls.rs.
// args: port cert.pem key.pem conns clientAuth(0-4) readyFile [clientCA.pem]
// Each connection: read one line, answer "hello from slang server: <line>",
// print a summary line, close.
async func run() void! {
    var a = os.Args()
    var cert = tls.LoadKeyPairFiles(a[2], a[3])?
    var cfg = tls.ServerConfig{Certificates: [cert], ALPN: ["http/1.1"], ClientAuth: strings.ToInt(a[5])?}
    if len(a) > 7 {
        var pool = x509.NewCertPool()
        pool.AppendPEM(io.ReadFile(a[7])?)
        cfg.ClientCAs = pool
    }
    var l = tls.Listen("127.0.0.1:" + a[1], cfg)?
    io.WriteFile(a[6], "ready")?
    var n = strings.ToInt(a[4])?
    for i in 0..n {
        var c = await l.Accept()?
        try {
            var got = ""
            for !strings.Contains(got, "\n") {
                var s = await c.ReadString(4096)?
                if s == "" {
                    break
                }
                got += s
            }
            var st = c.ConnectionState()
            var peer = "none"
            if len(st.PeerCertificates) > 0 {
                peer = st.PeerCertificates[0].Subject.CommonName
            }
            _ = await c.WriteString("hello from slang server: " + strings.Trim(got) + "\n")?
            println("conn", i + 1, st.VersionName(), st.CipherSuiteName, "resumed=" + fmt.Format("{0}", st.Resumed),
                "alpn=" + st.ALPN, "sni=" + st.ServerName, "peer=" + peer, "got=" + strings.Trim(got))
        } catch err {
            println("conn", i + 1, "error:", err)
        }
        c.Close()
    }
    l.Close()
}

async func main() {
    try await run() catch err {
        println("error:", err)
    }
}
