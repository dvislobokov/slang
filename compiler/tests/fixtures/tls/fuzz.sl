namespace tlstest.fuzz

import std.io
import std
import std.net
import std.crypto

// Fuzz smoke test for compiler/tests/tls.rs: mutated and random inputs to
// the X.509/PEM/private-key parsers, the record opener, and TLS servers and
// clients talking to peers that send garbage. Nothing may panic or leak;
// every input is either accepted or rejected with an error.

const leafPEM = `-----BEGIN CERTIFICATE-----
MIICujCCAaKgAwIBAgIUdS+lqrpbi21LXEqyZdXdIhI1ztUwDQYJKoZIhvcNAQEL
BQAwMjEgMB4GA1UEAwwXU2xhbmcgVGVzdCBJbnRlcm1lZGlhdGUxDjAMBgNVBAoM
BVNsYW5nMCAXDTI2MTAyMDE4NDUyMloYDzIxMjYwOTI2MTg0NTIyWjAaMRgwFgYD
VQQDDA93d3cuZXhhbXBsZS5jb20wWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATE
BJNcd7RGyfKtLEtyiKKs2NYzHs+psDMsvrOxPcPp1qcfZusI5qZ0e+XABAP/WC8M
YPEaThK0wZyW/Xfslx40o4GoMIGlMAkGA1UdEwQCMAAwDgYDVR0PAQH/BAQDAgeA
MBMGA1UdJQQMMAoGCCsGAQUFBwMBMDMGA1UdEQQsMCqCD3d3dy5leGFtcGxlLmNv
bYIRKi5hcGkuZXhhbXBsZS5jb22HBH8AAAEwHwYDVR0jBBgwFoAU990h2xsQus8r
MAg/vQhCo29QGoowHQYDVR0OBBYEFMqRK/5W4b1ccxoL42RctPqeYFfSMA0GCSqG
SIb3DQEBCwUAA4IBAQDIh2HjxJRZEBCSkZWRq0tEYXso9ISoKPxdNuIhy5MoA6zp
djjy8Qly70zJEmnOPj1N6scLJ78+2bJZsUJGdUqlRhZ9h+Q37pZyyMF8G0z+xuzG
FvF95x7bA29jidAGwTLw9VAwc40NBm3OrpEI+I3IAxL+uQ6CPpyVO4ov3GmGX5cx
zPoBRcqy2JEhavCd3wCv8fjwOZ1gx6Ac42BrGRLVYNBZccKNkEak6BOE4tiJsAte
FDbGDeK60MX25nLF+2H79hx5Cvia84bt5Vzs4s1pbUSdaDqaAVjPY2U33pIE7kaR
kqj42k3MedH8fYmyo0cx3dfNR1GMxpuYQgGqz3qB
-----END CERTIFICATE-----`

const interPEM = `-----BEGIN CERTIFICATE-----
MIICvzCCAmWgAwIBAgIUc8cZK8vXLXjqQVs7y79Judx/sq0wCgYIKoZIzj0EAwIw
LTEbMBkGA1UEAwwSU2xhbmcgVGVzdCBSb290IENBMQ4wDAYDVQQKDAVTbGFuZzAg
Fw0yNjEwMjAxODQ1MjJaGA8yMTI2MDkyNjE4NDUyMlowMjEgMB4GA1UEAwwXU2xh
bmcgVGVzdCBJbnRlcm1lZGlhdGUxDjAMBgNVBAoMBVNsYW5nMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAyydZiZTwIW2JBM5guu3gF47Q/wEQi05ikQJi
zZr74buRY3nxC0WuH9OUrjZqjuSNTOEIyx2a5DJy4PdGth9SK+5Hz095xUMxBgn8
qFGXne1Fsy7N92cKPDrSWvhFzjcyZuH/HdA6Tc/5JkUuqz2GC5Q3IcXjgrx+at54
vsalLsmVg4UMehrZxgAdR8MZZ4EzCO3dP9Xk/CNFSZwC9PkY5dfRGoImMUXH7xOx
B3RGq24rEMRPIdy3RSoJwoZUokUhwk4dVrzHmdJjR6AMY/HeE6jDGpEZS9Y9+u+T
gAAR5dbYLG0GkvuhJfVRnSmNlhch5siyQZKi/vifPye6+/sqnQIDAQABo4GQMIGN
MBIGA1UdEwEB/wQIMAYBAf8CAQAwDgYDVR0PAQH/BAQDAgEGMCcGA1UdHgQgMB6g
HDANggtleGFtcGxlLmNvbTALgglsb2NhbGhvc3QwHQYDVR0OBBYEFPfdIdsbELrP
KzAIP70IQqNvUBqKMB8GA1UdIwQYMBaAFM7uP6POhS3wOd2ypyENtpvGwlE7MAoG
CCqGSM49BAMCA0gAMEUCIQD/T+H45Wb6n5fkTSvMRL2TkLGx5hdp6mTeXyQEBSjm
1QIgdMihyt40ScWc1Ym43IAUkPD9xoOmK0+sSeeSLBFj1j8=
-----END CERTIFICATE-----`

const certPEM = `-----BEGIN CERTIFICATE-----
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

const keyPEM = `-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgjA2gD248y45QVG96
fXrFkd1iVNl1OOz2VN/OkZMN5jehRANCAASc1zlpmSuVZO3wBCodf/2ckVINRHTU
mOSXBzpVELQe6p45BK0zK1pyWibOTfHSGzqN94wNd2hMhWaKQIs0wMaQ
-----END PRIVATE KEY-----`

// A copy of b with a few random changes (flip, overwrite, cut, extend).
func mutate(b []byte) []byte {
    var out []byte = []
    for x in b {
        out = append(out, x)
    }
    var k = random.IntN(4) + 1
    for k > 0 {
        k -= 1
        var op = random.IntN(4)
        if len(out) == 0 {
            out = append(out, byte(random.IntN(256)))
        } else if op == 0 {
            var i = random.IntN(len(out))
            out[i] = out[i] ^ byte(1 << random.IntN(8))
        } else if op == 1 {
            out[random.IntN(len(out))] = byte(random.IntN(256))
        } else if op == 2 {
            out = out[..random.IntN(len(out))]
        } else {
            out = append(out, byte(random.IntN(256)))
        }
    }
    return out
}

func randomBytes(n int) []byte {
    var out []byte = []
    for len(out) < n {
        out = append(out, byte(random.IntN(256)))
    }
    return out
}

func fuzzX509() void! {
    var seeds []([]byte) = []
    for b in x509.DecodePEM(leafPEM + "\n" + interPEM + "\n" + certPEM + "\n" + keyPEM)? {
        seeds = append(seeds, b.Bytes)
    }
    var parsed = 0
    var rejected = 0
    for i in 0..3000 {
        var input = mutate(seeds[i % len(seeds)])
        if i % 7 == 0 {
            input = randomBytes(random.IntN(300))
        }
        var c = try x509.ParseDER(input) else nil
        if c != nil {
            parsed += 1
            try c.VerifyHostname("www.example.com") catch err {
                rejected += 0
            }
            _ = c.Subject.String()
        } else {
            rejected += 1
        }
        _ = try x509.ParsePrivateKey(input) else x509.PrivateKey{}
        var text = "-----BEGIN CERTIFICATE-----\n" + encoding.base64.Encode(input) + "\n-----END CERTIFICATE-----\n"
        if i % 5 == 0 {
            text = strings.FromBytes(input)
        }
        _ = try x509.ParsePEM(text) else []
    }
    println("x509: 3000 inputs,", parsed + rejected, "handled")
}

func fuzzRecords() void! {
    var key = randomBytes(16)
    var iv = randomBytes(12)
    var good = tls.SealRecord(0x1301, key, iv, 0, 23, strings.Bytes("payload"))?
    var opened = 0
    for i in 0..2000 {
        var rec = mutate(good)
        if i % 3 == 0 {
            rec = randomBytes(random.IntN(64))
        }
        var r = try tls.OpenRecord(0x1301, key, iv, 0, rec) else []
        if len(r) > 0 && encoding.hex.Encode(rec) != encoding.hex.Encode(good) {
            opened += 1
        }
    }
    println("records: 2000 inputs, forgeries accepted:", opened)
}

// A raw TCP peer that sends garbage (sometimes behind a plausible header).
async func garbagePeer(c net.Conn, round int) {
    var data = randomBytes(random.IntN(600) + 1)
    if round % 2 == 0 {
        // a handshake record header followed by random handshake bytes
        var hdr = [byte(22), byte(3), byte(1), byte(len(data) >> 8), byte(len(data) & 255)]
        _ = try await c.Write(hdr) else 0
    }
    _ = try await c.Write(data) else 0
    // closing at once: a peer that waits would deadlock on garbage that
    // announces more bytes than it carries
    c.Close()
}

async func fuzzServer(cert tls.Certificate) void! {
    var tl = tls.Listen("127.0.0.1:0", tls.ServerConfig{Certificates: [cert]})?
    var addr = fmt.Format("127.0.0.1:{0}", tl.Port())
    var failed = 0
    for i in 0..150 {
        var raw = await net.Dial(addr)?
        var peer = garbagePeer(raw, i)
        var c = await tl.Accept()?
        try await c.Handshake() catch err {
            failed += 1
        }
        c.Close()
        await peer
    }
    tl.Close()
    println("server: 150 garbage clients, handshakes failed:", failed)
}

async func fuzzClient() void! {
    var l = net.Listen("127.0.0.1:0")?
    var addr = fmt.Format("127.0.0.1:{0}", l.Port())
    var failed = 0
    for i in 0..150 {
        var dial = tls.DialTimeout(addr, tls.Config{InsecureSkipVerify: true}, 5000)
        var raw = await l.Accept()?
        var peer = garbagePeer(raw, i)
        var c = try await dial catch err {
            failed += 1
            await peer
            continue
        }
        c.Close()
        await peer
    }
    l.Close()
    println("client: 150 garbage servers, handshakes failed:", failed)
}

async func main() void! {
    random.Seed(8448)
    fuzzX509()?
    fuzzRecords()?
    var cert = tls.LoadKeyPair(certPEM, keyPEM)?
    await fuzzServer(cert)?
    await fuzzClient()?
}
