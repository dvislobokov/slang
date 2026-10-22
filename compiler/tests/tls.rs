//! TLS end-to-end tests (`std.net.tls`, `std.crypto.x509`):
//!
//! * interop with OpenSSL in both directions: the Zell client against
//!   `openssl s_server` restricted to TLS 1.3 and to TLS 1.2 (several
//!   cipher suites, ECDSA and RSA certificates, HelloRetryRequest,
//!   resumption from tickets, client certificates), and `openssl s_client
//!   -tls1_3` / `-tls1_2` against the Zell `tls.Server` (resumption with
//!   `-sess_out`/`-sess_in`, HelloRetryRequest, client certificates, the
//!   TLS 1.2 downgrade sentinel);
//! * HTTPS: the `std.net.http` client against `openssl s_server -www`,
//!   and `curl` against `http.Server{TLS}`;
//! * `examples/tls_echo.zl` (Zell to Zell, several worker counts);
//! * a fuzz smoke test feeding mutated and random input to the X.509
//!   parser, the record layer and handshakes (no panics, no leaks).
//!
//! Every Zell program is built with `--leak-check`. The certificates are
//! generated with `openssl` into a temporary directory; without `openssl`
//! the interop tests are skipped (with a note), the rest still run.
//! The byte-exact trace tests are spec tests: `tests/spec/stdlib/tls_rfc8448.zl`
//! (RFC 8448 key schedule, records, PSK binder) and `tls_trace13.zl` (a full
//! TLS 1.3 handshake capture).

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn ensure_runtime() {
    if !repo().join("runtime/libzell_rt.a").is_file() {
        let st = Command::new("make").arg("-C").arg(repo().join("runtime")).status().expect("cannot run make");
        assert!(st.success(), "make -C runtime failed");
    }
}

fn have_openssl() -> bool {
    Command::new("openssl").arg("version").output().is_ok_and(|o| o.status.success())
}

/// Builds `src` with `--leak-check` into `dir`.
fn build(src: &Path, dir: &Path, name: &str) -> PathBuf {
    ensure_runtime();
    let exe = dir.join(name);
    let b = Command::new(env!("CARGO_BIN_EXE_zell")).arg("build").arg(src).arg("-o").arg(&exe).arg("--leak-check").output().unwrap();
    assert!(b.status.success(), "build of {} failed:\n{}", src.display(), String::from_utf8_lossy(&b.stderr));
    exe
}

/// Kills the child when dropped.
struct Proc(Child);

impl Drop for Proc {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

/// Runs a command to completion with a deadline; returns (exit code, stdout, stderr).
fn run(cmd: &mut Command, limit: Duration) -> (Option<i32>, String, String) {
    let mut child = cmd.env("ZELL_NO_BACKTRACE", "1").stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().expect("cannot start");
    let start = Instant::now();
    loop {
        if child.try_wait().unwrap().is_some() {
            break;
        }
        if start.elapsed() > limit {
            let _ = child.kill();
            let out = child.wait_with_output().unwrap();
            panic!("{cmd:?} did not finish within {limit:?}; stdout:\n{}\nstderr:\n{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let out = child.wait_with_output().unwrap();
    (out.status.code(), String::from_utf8_lossy(&out.stdout).into_owned(), String::from_utf8_lossy(&out.stderr).into_owned())
}

fn free_port() -> u16 {
    TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
}

fn wait_port(port: u16) {
    let start = Instant::now();
    while TcpStream::connect(("127.0.0.1", port)).is_err() {
        assert!(start.elapsed() < Duration::from_secs(10), "nothing listens on port {port}");
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn wait_file(p: &Path) {
    let start = Instant::now();
    while !p.exists() {
        assert!(start.elapsed() < Duration::from_secs(20), "server did not start ({} missing)", p.display());
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// Generated certificates and the built interop programs.
struct Fixture {
    dir: PathBuf,
    client: PathBuf,
    server: PathBuf,
    https: PathBuf,
}

fn openssl(dir: &Path, args: &[&str]) {
    let out = Command::new("openssl").current_dir(dir).args(args).output().unwrap();
    assert!(out.status.success(), "openssl {args:?} failed:\n{}", String::from_utf8_lossy(&out.stderr));
}

fn fixture() -> &'static Fixture {
    static F: OnceLock<Fixture> = OnceLock::new();
    F.get_or_init(|| {
        let dir = std::env::temp_dir().join(format!("zell-tls-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let san = "subjectAltName=DNS:localhost,IP:127.0.0.1";
        openssl(&dir, &["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", "ec.key", "-out", "ec.pem", "-days", "3650", "-subj", "/CN=localhost", "-addext", san]);
        openssl(&dir, &["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "rsa.key", "-out", "rsa.pem", "-days", "3650", "-subj", "/CN=localhost", "-addext", san]);
        openssl(&dir, &["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", "client.key", "-out", "client.pem", "-days", "3650", "-subj", "/CN=interop client", "-addext", "extendedKeyUsage=clientAuth"]);
        let fx = repo().join("compiler/tests/fixtures/tls");
        let client = build(&fx.join("client.zl"), &dir, "client");
        let server = build(&fx.join("server.zl"), &dir, "server");
        let https = build(&fx.join("https.zl"), &dir, "https");
        Fixture { dir, client, server, https }
    })
}

macro_rules! need_openssl {
    () => {
        if !have_openssl() {
            eprintln!("skipping: openssl not found");
            return;
        }
    };
}

/// `openssl s_server -rev` (answers every line reversed; `-www` in `extra`
/// serves a status page instead) on a free port.
fn s_server(f: &Fixture, cert: &str, extra: &[&str]) -> (Proc, u16) {
    let port = free_port();
    let child = Command::new("openssl")
        .current_dir(&f.dir)
        .args(["s_server", "-accept", &port.to_string(), "-cert", &format!("{cert}.pem"), "-key", &format!("{cert}.key"), "-naccept", "100"])
        .args(if extra.contains(&"-www") { &[][..] } else { &["-rev"][..] })
        .args(extra)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let p = Proc(child);
    wait_port(port);
    (p, port)
}

/// Runs the Zell client; returns its output lines.
fn zell_client(f: &Fixture, port: u16, roots: &str, version: &str, dials: usize, extra: &[&str]) -> Vec<String> {
    let mut cmd = Command::new(&f.client);
    cmd.current_dir(&f.dir).arg(format!("127.0.0.1:{port}")).arg(roots).arg(version).arg(dials.to_string()).arg("hello tls").args(extra);
    let (code, out, err) = run(&mut cmd, Duration::from_secs(30));
    assert_eq!(code, Some(0), "client failed: stdout:\n{out}\nstderr:\n{err}");
    assert!(err.is_empty() && !out.contains("error:"), "client failed: stdout:\n{out}\nstderr:\n{err}");
    out.lines().map(str::to_string).collect()
}

fn check_dials(lines: &[String], version: &str, suite: &str, resumes: bool) {
    assert!(!lines.is_empty());
    for (i, l) in lines.iter().enumerate() {
        assert!(l.contains(version), "{l}");
        assert!(l.contains(suite), "{l} (want {suite})");
        assert!(l.contains("reply=slt olleh"), "{l}");
        let resumed = if i > 0 && resumes { "resumed=true" } else { "resumed=false" };
        assert!(l.contains(resumed), "dial {}: {l}", i + 1);
    }
}

#[test]
fn zell_client_against_openssl_tls13() {
    need_openssl!();
    let f = fixture();
    let cases: &[(&str, &[&str], &str)] = &[
        ("ec", &["-tls1_3"], "TLS_AES_"),
        ("rsa", &["-tls1_3"], "TLS_AES_"),
        // the client offers an X25519 share; P-256 only forces a HelloRetryRequest
        ("ec", &["-tls1_3", "-groups", "P-256"], "TLS_AES_"),
        ("ec", &["-tls1_3", "-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256"], "TLS_CHACHA20_POLY1305_SHA256"),
        ("rsa", &["-tls1_3", "-ciphersuites", "TLS_AES_256_GCM_SHA384"], "TLS_AES_256_GCM_SHA384"),
        ("ec", &["-tls1_3", "-ciphersuites", "TLS_AES_128_GCM_SHA256", "-alpn", "http/1.1"], "TLS_AES_128_GCM_SHA256"),
    ];
    for (cert, extra, suite) in cases {
        let (_srv, port) = s_server(f, cert, extra);
        let lines = zell_client(f, port, &format!("{cert}.pem"), "any", 3, &[]);
        check_dials(&lines, "TLS 1.3", suite, true);
        assert!(lines[0].contains("verified=1"), "{}", lines[0]);
        if extra.contains(&"-alpn") {
            assert!(lines[0].contains("alpn=http/1.1"), "{}", lines[0]);
        }
    }
}

#[test]
fn zell_client_against_openssl_tls12() {
    need_openssl!();
    let f = fixture();
    let cases: &[(&str, &[&str], &str)] = &[
        ("ec", &["-tls1_2"], "TLS_ECDHE_ECDSA_WITH_"),
        ("rsa", &["-tls1_2"], "TLS_ECDHE_RSA_WITH_"),
        ("ec", &["-tls1_2", "-cipher", "ECDHE-ECDSA-AES256-GCM-SHA384", "-groups", "P-256"], "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384"),
        ("rsa", &["-tls1_2", "-cipher", "ECDHE-RSA-CHACHA20-POLY1305"], "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256"),
        ("ec", &["-tls1_2", "-cipher", "ECDHE-ECDSA-CHACHA20-POLY1305"], "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256"),
        ("rsa", &["-tls1_2", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256", "-sigalgs", "RSA+SHA256"], "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256"),
    ];
    for (cert, extra, suite) in cases {
        let (_srv, port) = s_server(f, cert, extra);
        // offering 1.3 too exercises the version fallback
        let lines = zell_client(f, port, &format!("{cert}.pem"), "any", 3, &[]);
        check_dials(&lines, "TLS 1.2", suite, true);
        let lines = zell_client(f, port, &format!("{cert}.pem"), "12", 1, &[]);
        check_dials(&lines, "TLS 1.2", suite, false);
    }
}

#[test]
fn zell_client_certificate_with_openssl() {
    need_openssl!();
    let f = fixture();
    for v in ["-tls1_3", "-tls1_2"] {
        let (_srv, port) = s_server(f, "ec", &[v, "-Verify", "1", "-CAfile", "client.pem"]);
        let lines = zell_client(f, port, "ec.pem", "any", 1, &["client.pem", "client.key"]);
        assert!(lines[0].contains("reply=slt olleh"), "{v}: {lines:?}");
    }
}

#[test]
fn zell_client_rejects_bad_certificates() {
    need_openssl!();
    let f = fixture();
    let (_srv, port) = s_server(f, "ec", &["-tls1_3"]);
    // the RSA certificate is not a root of the EC server's chain
    let mut cmd = Command::new(&f.client);
    cmd.current_dir(&f.dir).arg(format!("127.0.0.1:{port}")).arg("rsa.pem").arg("any").arg("1").arg("x");
    let (code, out, err) = run(&mut cmd, Duration::from_secs(30));
    assert_eq!(code, Some(0), "stderr:\n{err}");
    assert!(out.starts_with("error: tls: server certificate: certificate signed by unknown authority"), "{out}");
}

/// Starts the Zell server for `conns` connections.
fn zell_server(f: &Fixture, cert: &str, conns: usize, client_auth: usize) -> (Child, u16) {
    let port = free_port();
    let ready = f.dir.join(format!("ready-{port}"));
    let mut cmd = Command::new(&f.server);
    cmd.current_dir(&f.dir)
        .args([port.to_string(), format!("{cert}.pem"), format!("{cert}.key"), conns.to_string(), client_auth.to_string()])
        .arg(&ready)
        .arg("client.pem")
        .env("ZELL_NO_BACKTRACE", "1")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let child = cmd.spawn().unwrap();
    wait_file(&ready);
    (child, port)
}

fn finish_server(child: Child) -> String {
    let mut child = child;
    let start = Instant::now();
    while child.try_wait().unwrap().is_none() {
        if start.elapsed() > Duration::from_secs(30) {
            let _ = child.kill();
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let out = child.wait_with_output().unwrap();
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success() && err.is_empty(), "server failed ({:?}):\n{err}\n{}", out.status, String::from_utf8_lossy(&out.stdout));
    String::from_utf8_lossy(&out.stdout).into_owned()
}

/// Runs `openssl s_client`, sends one line and waits for the answer.
fn s_client(f: &Fixture, port: u16, extra: &[&str], line: &str) -> String {
    let mut child = Command::new("openssl")
        .current_dir(&f.dir)
        .args(["s_client", "-connect", &format!("127.0.0.1:{port}"), "-servername", "localhost"])
        .args(extra)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    stdin.write_all(format!("{line}\n").as_bytes()).unwrap();
    let mut stdout = child.stdout.take().unwrap();
    let reader = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = stdout.read_to_string(&mut s);
        s
    });
    // s_client exits when the server closes the connection
    let start = Instant::now();
    while child.try_wait().unwrap().is_none() {
        if start.elapsed() > Duration::from_secs(20) {
            let _ = child.kill();
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    drop(stdin);
    let _ = child.wait();
    reader.join().unwrap()
}

#[test]
fn openssl_client_against_zell_server() {
    need_openssl!();
    let f = fixture();
    let cases: &[(&str, &[&str], &str)] = &[
        ("ec", &["-tls1_3"], "TLS 1.3"),
        ("rsa", &["-tls1_3"], "TLS 1.3"),
        ("ec", &["-tls1_2"], "TLS 1.2"),
        ("rsa", &["-tls1_2"], "TLS 1.2"),
        ("ec", &["-tls1_3", "-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256"], "TLS_CHACHA20_POLY1305_SHA256"),
        ("ec", &["-tls1_3", "-ciphersuites", "TLS_AES_256_GCM_SHA384", "-alpn", "http/1.1"], "TLS_AES_256_GCM_SHA384"),
        ("rsa", &["-tls1_2", "-cipher", "ECDHE-RSA-CHACHA20-POLY1305"], "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256"),
        ("ec", &["-tls1_2", "-cipher", "ECDHE-ECDSA-AES256-GCM-SHA384", "-groups", "P-256"], "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384"),
        // only an X448 share (not supported here): the server sends a HelloRetryRequest for X25519
        ("ec", &["-tls1_3", "-groups", "X448:X25519"], "TLS 1.3"),
        ("ec", &["-tls1_3", "-groups", "P-256"], "TLS 1.3"),
    ];
    for (cert, extra, want) in cases {
        let (srv, port) = zell_server(f, cert, 1, 0);
        let out = s_client(f, port, extra, "ping");
        assert!(out.contains("hello from zell server: ping"), "{extra:?}: s_client output:\n{out}");
        let log = finish_server(srv);
        assert!(log.contains(want) && log.contains("got=ping") && log.contains("sni=localhost"), "{extra:?}: {log}");
        if extra.contains(&"-alpn") {
            assert!(log.contains("alpn=http/1.1"), "{log}");
        }
    }
}

#[test]
fn openssl_client_resumes_zell_sessions() {
    need_openssl!();
    let f = fixture();
    for (v, name) in [("-tls1_3", "TLSv1.3"), ("-tls1_2", "TLSv1.2")] {
        let (srv, port) = zell_server(f, "ec", 2, 0);
        let sess = f.dir.join(format!("sess-{port}.pem"));
        let sess = sess.to_str().unwrap();
        let first = s_client(f, port, &[v, "-sess_out", sess], "one");
        assert!(first.contains("hello from zell server: one"), "{first}");
        let second = s_client(f, port, &[v, "-sess_in", sess], "two");
        assert!(second.contains(&format!("Reused, {name}")), "{v}: {second}");
        let log = finish_server(srv);
        assert!(log.contains("conn 1") && log.contains("resumed=false"), "{log}");
        assert!(log.lines().nth(1).is_some_and(|l| l.contains("resumed=true")), "{log}");
    }
}

#[test]
fn openssl_client_certificates_with_zell_server() {
    need_openssl!();
    let f = fixture();
    for v in ["-tls1_3", "-tls1_2"] {
        // RequireAndVerifyClientCert, client.pem is the only client CA
        let (srv, port) = zell_server(f, "ec", 2, 4);
        let out = s_client(f, port, &[v, "-cert", "client.pem", "-key", "client.key"], "auth");
        assert!(out.contains("hello from zell server: auth"), "{v}: {out}");
        let _ = s_client(f, port, &[v], "anon");
        let log = finish_server(srv);
        assert!(log.contains("peer=interop client"), "{v}: {log}");
        assert!(log.contains("conn 2 error: tls: client certificate required") || log.contains("conn 2 error:"), "{v}: {log}");
    }
}

#[test]
fn zell_server_sets_the_downgrade_sentinel() {
    need_openssl!();
    let f = fixture();
    let (srv, port) = zell_server(f, "ec", 1, 0);
    // a TLS 1.2-only client sees "DOWNGRD\x01" at the end of ServerHello.random
    let out = s_client(f, port, &["-tls1_2", "-msg"], "x");
    let _ = finish_server(srv);
    let flat: String = out.split_whitespace().collect::<Vec<_>>().join(" ");
    assert!(flat.contains("44 4f 57 4e 47 52 44 01"), "no downgrade sentinel in:\n{out}");
}

#[test]
fn tls_echo_example() {
    ensure_runtime();
    let src = repo().join("examples/tls_echo.zl");
    let want = std::fs::read_to_string(src.with_extension("out")).unwrap();
    let dir = std::env::temp_dir().join(format!("zell-tls-echo-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let exe = build(&src, &dir, "tls_echo");
    for workers in ["1", "2", "8"] {
        let (code, out, err) = run(Command::new(&exe).env("ZELL_WORKERS", workers), Duration::from_secs(60));
        assert_eq!(code, Some(0), "ZELL_WORKERS={workers}: stderr:\n{err}");
        assert!(err.is_empty(), "ZELL_WORKERS={workers}: stderr:\n{err}");
        assert_eq!(out, want, "ZELL_WORKERS={workers}: output differs");
    }
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn fuzz_smoke() {
    ensure_runtime();
    let dir = std::env::temp_dir().join(format!("zell-tls-fuzz-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let exe = build(&repo().join("compiler/tests/fixtures/tls/fuzz.zl"), &dir, "fuzz");
    let (code, out, err) = run(&mut Command::new(&exe), Duration::from_secs(120));
    assert_eq!(code, Some(0), "fuzz failed:\nstdout:\n{out}\nstderr:\n{err}");
    assert!(err.is_empty(), "stderr:\n{err}");
    assert!(out.contains("x509: 3000 inputs, 3000 handled"), "{out}");
    assert!(out.contains("records: 2000 inputs, forgeries accepted: 0"), "{out}");
    assert!(out.contains("server: 150 garbage clients, handshakes failed: 150"), "{out}");
    assert!(out.contains("client: 150 garbage servers, handshakes failed: 150"), "{out}");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn https_client_against_openssl_www() {
    need_openssl!();
    let f = fixture();
    for v in ["-tls1_3", "-tls1_2"] {
        let (_srv, port) = s_server(f, "rsa", &[v, "-www"]);
        let mut cmd = Command::new(&f.https);
        cmd.current_dir(&f.dir).args(["get", &format!("https://localhost:{port}/"), "rsa.pem"]);
        let (code, out, err) = run(&mut cmd, Duration::from_secs(30));
        assert!(code == Some(0) && err.is_empty(), "{out}\n{err}");
        assert!(out.starts_with("status 200 body ") && out.trim_end().ends_with("true"), "{v}: {out}");
    }
}

#[test]
fn curl_against_zell_https_server() {
    need_openssl!();
    if !Command::new("curl").arg("--version").output().is_ok_and(|o| o.status.success()) {
        eprintln!("skipping: curl not found");
        return;
    }
    let f = fixture();
    let port = free_port();
    let ready = f.dir.join(format!("https-ready-{port}"));
    let child = Command::new(&f.https)
        .current_dir(&f.dir)
        .args(["serve", &port.to_string(), "ec.pem", "ec.key"])
        .arg(&ready)
        .arg("2")
        .env("ZELL_NO_BACKTRACE", "1")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    wait_file(&ready);
    let url = format!("https://localhost:{port}/hello");
    for tls in [&["--tlsv1.3"][..], &["--tlsv1.2", "--tls-max", "1.2"][..]] {
        let (code, out, err) = run(Command::new("curl").current_dir(&f.dir).args(["-sS", "--noproxy", "*", "--cacert", "ec.pem"]).args(tls).arg(&url), Duration::from_secs(30));
        assert_eq!(code, Some(0), "curl {tls:?}: {err}");
        assert_eq!(out, "hello over https from zell\n", "curl {tls:?}");
    }
    let log = finish_server(child);
    assert_eq!(log, "served 2\n");
}

