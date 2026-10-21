// test: std.crypto.x509 parses certificates and keys (PEM/DER) and verifies chains: SAN/wildcard/IP host names, validity, EKU, CA flags, name constraints
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.x509_verify

import std
import std.crypto
import std.io

// A P-256 root, an RSA intermediate (pathlen 0, name constraints
// example.com and localhost) and a P-256 leaf for www.example.com,
// *.api.example.com and 127.0.0.1, valid 2026-10-20 .. 2126.
const rootPEM = `-----BEGIN CERTIFICATE-----
MIIBwTCCAWegAwIBAgIUA9Sb9VwlxjjxD1YgDii/0p4QUi0wCgYIKoZIzj0EAwIw
LTEbMBkGA1UEAwwSU2xhbmcgVGVzdCBSb290IENBMQ4wDAYDVQQKDAVTbGFuZzAg
Fw0yNjEwMjAxODQ1MjJaGA8yMTI2MDkyNjE4NDUyMlowLTEbMBkGA1UEAwwSU2xh
bmcgVGVzdCBSb290IENBMQ4wDAYDVQQKDAVTbGFuZzBZMBMGByqGSM49AgEGCCqG
SM49AwEHA0IABFDrPW3OpDMA75Eh6VDthmLNoJtR/CBrzPbIhbJzy7Aw/1hriV4d
aJtqjavruWAcWp1SWiILBYyo0ygoCfMh6TGjYzBhMB0GA1UdDgQWBBTO7j+jzoUt
8DndsqchDbabxsJROzAfBgNVHSMEGDAWgBTO7j+jzoUt8DndsqchDbabxsJROzAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBBjAKBggqhkjOPQQDAgNIADBF
AiAL28cEdwXkr29xr5XtgDdZV+Pv0iEN8370qk9jyU9qGgIhAMWLjK+gut52keiE
+ZySEyT9u4w61+06l2gqhfxqWrLt
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

// Issued by the intermediate for evil.org, outside its name constraints.
const evilPEM = `-----BEGIN CERTIFICATE-----
MIICgTCCAWmgAwIBAgIUdS+lqrpbi21LXEqyZdXdIhI1ztYwDQYJKoZIhvcNAQEL
BQAwMjEgMB4GA1UEAwwXU2xhbmcgVGVzdCBJbnRlcm1lZGlhdGUxDjAMBgNVBAoM
BVNsYW5nMCAXDTI2MTAyMDE5MjAwMFoYDzIxMjYwOTI2MTkyMDAwWjATMREwDwYD
VQQDDAhldmlsLm9yZzBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABMQEk1x3tEbJ
8q0sS3KIoqzY1jMez6mwMyy+s7E9w+nWpx9m6wjmpnR75cAEA/9YLwxg8RpOErTB
nJb9d+yXHjSjdzB1MAkGA1UdEwQCMAAwEwYDVR0lBAwwCgYIKwYBBQUHAwEwEwYD
VR0RBAwwCoIIZXZpbC5vcmcwHQYDVR0OBBYEFMqRK/5W4b1ccxoL42RctPqeYFfS
MB8GA1UdIwQYMBaAFPfdIdsbELrPKzAIP70IQqNvUBqKMA0GCSqGSIb3DQEBCwUA
A4IBAQCj8woqFRc2HME68ncY1PCJTY9HiITlX4IMT7YsdMriTrxxbg08xjfxbPAp
0k4Ic7GYwzztMM5xk8ZT0rhIjZfllv3k/cbT3q/eDbRFGAdO2jIt37Mt26J8/g69
X03/fPz2MJUfR3E1IuumLtRsS5k5iuA/SNhIgCxTJ7F/8fUrwxtZGwxCGrtNcmVZ
BO5AlvdcPpO8KvXbEci3ED7g2SA/PhjlQ9kpSAb+6jfVjnk5UGa7oie6zIbdkkoI
N2QtxVPOTGCwX1uWkxJnop/Z5HHkU73eJuDQvcGkJ/Y6NOQzOceQkdjUXly4dJ7B
xuSrisWAKawg1ae+9kXzLRSLapfX
-----END CERTIFICATE-----`

const leafKey = `-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIAGsc29bth8PGwXhidNXSRXaTopYnKsTbhwVo65AEA6EoAoGCCqGSM49
AwEHoUQDQgAExASTXHe0RsnyrSxLcoiirNjWMx7PqbAzLL6zsT3D6danH2brCOam
dHvlwAQD/1gvDGDxGk4StMGclv137JceNA==
-----END EC PRIVATE KEY-----`

func check(leaf *x509.Certificate, opts x509.VerifyOptions, what string) {
    var chain = try x509.Verify(leaf, opts) catch err {
        println(what + ":", err)
        return
    }
    var names []string = []
    for c in chain {
        names = append(names, c.Subject.CommonName)
    }
    println(what + ": ok", strings.Join(names, " <- "))
}

func main() void! {
    var root = x509.ParsePEM(rootPEM)?[0]
    var inter = x509.ParsePEM(interPEM)?[0]
    var leaf = x509.ParsePEM(leafPEM)?[0]
    println("subject:", leaf.Subject.String(), "issuer:", leaf.Issuer.String())
    println(
        "version:",
        leaf.Version,
        "sig:",
        leaf.SignatureAlgorithm,
        "key:",
        leaf.PublicKeyAlgorithm,
        leaf.Curve,
        len(leaf.PublicKey),
    )
    println(
        "san:",
        leaf.DNSNames,
        leaf.IPAddresses,
        "eku:",
        leaf.ExtKeyUsage,
        "ku:",
        leaf.KeyUsage,
        "ca:",
        leaf.IsCA,
    )
    println(
        "inter:",
        inter.PublicKeyAlgorithm,
        len(inter.PublicKey) * 8,
        "bits, ca:",
        inter.IsCA,
        "pathlen:",
        inter.MaxPathLen,
        "permitted:",
        inter.PermittedDNSDomains,
    )
    println(
        "root:",
        root.Subject.CommonName,
        "ca:",
        root.IsCA,
        "self-signed:",
        root.Subject.String() == root.Issuer.String(),
    )
    println(
        "not before:",
        time.Format(leaf.NotBefore, "%Y-%m-%d"),
        "not after:",
        time.Format(leaf.NotAfter, "%Y-%m-%d"),
    )
    println(
        "aki = parent ski:",
        encoding.hex.Encode(leaf.AuthorityKeyId) == encoding.hex.Encode(inter.SubjectKeyId),
    )
    var der = x509.ParseDER(leaf.Raw)?
    println("der round trip:", der.Equal(leaf), len(der.Fingerprint()))
    var pem = x509.EncodePEM("CERTIFICATE", leaf.Raw)
    println("pem round trip:", x509.ParsePEM(pem)?[0].Equal(leaf))

    var pool = x509.NewCertPool()
    pool.AddCert(root)
    var t2030 = time.Parse("2030-01-01", "%Y-%m-%d")?
    var base = x509.VerifyOptions{Roots: pool, Intermediates: [inter], CurrentTime: t2030}
    var o = base
    o.DNSName = "www.example.com"
    check(leaf, o, "www.example.com")
    o.DNSName = "WWW.Example.COM."
    check(leaf, o, "case and trailing dot")
    o.DNSName = "v1.api.example.com"
    check(leaf, o, "wildcard")
    o.DNSName = "a.b.api.example.com"
    check(leaf, o, "wildcard two labels")
    o.DNSName = "api.example.com"
    check(leaf, o, "wildcard apex")
    o.DNSName = "127.0.0.1"
    check(leaf, o, "ip")
    o.DNSName = "10.0.0.1"
    check(leaf, o, "other ip")
    o.DNSName = ""
    o.KeyUsage = "clientAuth"
    check(leaf, o, "client auth")
    var noInter = base
    noInter.Intermediates = []
    check(leaf, noInter, "missing intermediate")
    var late = base
    late.CurrentTime = time.Parse("2200-01-01", "%Y-%m-%d")?
    check(leaf, late, "in 2200")
    var early = base
    early.CurrentTime = time.Parse("2001-01-01", "%Y-%m-%d")?
    check(leaf, early, "in 2001")
    var other = base
    other.Roots = x509.NewCertPool()
    check(leaf, other, "empty pool")
    var selfRoot = base
    selfRoot.DNSName = ""
    check(root, selfRoot, "root itself")
    // the intermediate has no extended key usage, so it passes as a leaf
    check(inter, base, "intermediate as leaf")
    var evil = x509.ParsePEM(evilPEM)?[0]
    check(evil, base, "name constraints")
    try leaf.CheckSignatureFrom(root) catch err {
        println("leaf signed by root:", err)
    }
    inter.CheckSignatureFrom(root)?
    println("intermediate signed by root: ok")

    var key = x509.ParsePrivateKeyPEM(leafKey)?
    println(
        "key:",
        key.Algorithm,
        len(key.Bytes),
        "matches leaf:",
        key.Matches(leaf),
        "matches inter:",
        key.Matches(inter),
    )
    try x509.ParsePEM("not a certificate") catch err {
        println("garbage:", err)
    }
    var blocks = x509.DecodePEM(rootPEM + "\n" + leafKey)?
    println("blocks:", len(blocks), blocks[0].Type, blocks[1].Type)
}
