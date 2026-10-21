// test: x509.SystemRoots is parsed once per process and reused while the bundle file is unchanged; each call returns its own pool; a changed bundle (size/mtime) is parsed again
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.x509_system_roots_cache

import std
import std.crypto
import std.io

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

func main() void! {
    var path = fs.Join(fs.TempDir(), fmt.Format("slang-roots-{0}.pem", os.Pid()))
    io.WriteFile(path, rootPEM + "\n")?
    os.Setenv("SSL_CERT_FILE", path)?
    var a = x509.SystemRoots()?
    println("first:", a.Len())
    // the caller's pool is its own: adding to it does not change the cache
    a.AppendPEM(interPEM)
    var b = x509.SystemRoots()?
    println("after adding to the first pool:", a.Len(), b.Len())
    // a cached call is much cheaper than parsing (timed over many calls)
    var t0 = time.Now()
    for _ in 0..200 {
        _ = x509.SystemRoots()?
    }
    println("200 cached calls under 1 s:", time.Now() - t0 < 1000)
    // a changed bundle is read again
    io.WriteFile(path, rootPEM + "\n" + interPEM + "\n")?
    var c = x509.SystemRoots()?
    println("changed bundle:", c.Len())
    fs.Remove(path)?
}
