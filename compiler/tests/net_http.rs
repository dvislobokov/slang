//! End-to-end tests of the async runtime pieces that talk to the network:
//! `examples/http_server.sl` (an HTTP server and client in one program),
//! `examples/tcp_echo.sl`, `tests/spec/stdlib/net_many_conns.sl` (up to
//! 10 000 concurrent connections) and
//! `examples/channels.sl`, `cancel.sl` and `task_context.sl` are built with `--leak-check` and run with a
//! deadline, with several worker counts, and must print their `.out` file
//! byte for byte and release every object (sockets, tasks, channels).
//!
//! The HTTP/2 and WebSocket interop tests at the end run the fixtures of
//! `compiler/tests/fixtures/h2` against `curl` (with nghttp2) and Go's
//! net/http when those are installed (otherwise they are skipped).
//!
//! Needs `clang` and `runtime/libslang_rt.a` (`make -C runtime`).

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn ensure_runtime() {
    if !repo().join("runtime/libslang_rt.a").is_file() {
        let st = Command::new("make").arg("-C").arg(repo().join("runtime")).status().expect("cannot run make");
        assert!(st.success(), "make -C runtime failed");
    }
}

/// Runs `exe` with `SLANG_WORKERS=workers`; kills it after `limit`.
fn run_with_deadline(exe: &Path, workers: &str, limit: Duration) -> (Option<i32>, String, String) {
    let mut child = Command::new(exe)
        .env("SLANG_NO_BACKTRACE", "1")
        .env("SLANG_WORKERS", workers)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("cannot start the example");
    let start = Instant::now();
    loop {
        if let Some(_status) = child.try_wait().unwrap() {
            break;
        }
        if start.elapsed() > limit {
            let _ = child.kill();
            let out = child.wait_with_output().unwrap();
            panic!(
                "{} did not finish within {limit:?} (SLANG_WORKERS={workers}); stdout so far:\n{}",
                exe.display(),
                String::from_utf8_lossy(&out.stdout)
            );
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let out = child.wait_with_output().unwrap();
    (out.status.code(), String::from_utf8_lossy(&out.stdout).into_owned(), String::from_utf8_lossy(&out.stderr).into_owned())
}

fn check_example(name: &str) {
    ensure_runtime();
    let src = repo().join("examples").join(format!("{name}.sl"));
    let want = std::fs::read_to_string(src.with_extension("out")).unwrap();
    let dir = std::env::temp_dir().join(format!("slang-net-{}-{name}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let exe = dir.join(name);
    let b = Command::new(env!("CARGO_BIN_EXE_slang")).arg("build").arg(&src).arg("-o").arg(&exe).arg("--leak-check").output().unwrap();
    assert!(b.status.success(), "build of {name} failed:\n{}", String::from_utf8_lossy(&b.stderr));
    for workers in ["1", "2", "8"] {
        for _ in 0..3 {
            let (code, out, err) = run_with_deadline(&exe, workers, Duration::from_secs(30));
            assert_eq!(code, Some(0), "{name} (SLANG_WORKERS={workers}) failed, stderr:\n{err}");
            assert!(err.is_empty(), "{name} (SLANG_WORKERS={workers}): unexpected stderr:\n{err}");
            assert_eq!(out, want, "{name} (SLANG_WORKERS={workers}): output differs");
        }
    }
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn http_server_example_runs_with_deadline() {
    check_example("http_server");
}

#[test]
fn channels_example_runs_with_deadline() {
    check_example("channels");
}

#[test]
fn cancel_example_runs_with_deadline() {
    check_example("cancel");
}

#[test]
fn task_context_example_runs_with_deadline() {
    check_example("task_context");
}

#[test]
fn tcp_echo_example_runs_with_deadline() {
    check_example("tcp_echo");
}

/// The hard limit on open files of this process (`/proc/self/limits`).
fn nofile_hard_limit() -> Option<u64> {
    let text = std::fs::read_to_string("/proc/self/limits").ok()?;
    let line = text.lines().find(|l| l.starts_with("Max open files"))?;
    let fields: Vec<&str> = line.split_whitespace().collect();
    // "Max open files <soft> <hard> files"
    fields.get(4).and_then(|h| h.parse().ok()).or(Some(u64::MAX))
}

/// `tests/spec/stdlib/net_many_conns.sl` with up to 10 000 concurrent
/// connections (two descriptors each: the runtime raises the soft limit
/// to the hard one), under `--leak-check`, with 1 and 4 workers.
#[test]
fn ten_thousand_concurrent_connections() {
    ensure_runtime();
    let n = match nofile_hard_limit() {
        Some(h) => ((h.saturating_sub(300)) / 2).min(10_000),
        None => 2_000,
    };
    if n < 1000 {
        eprintln!("skipping: open-files hard limit too low");
        return;
    }
    let src = repo().join("tests/spec/stdlib/net_many_conns.sl");
    let dir = std::env::temp_dir().join(format!("slang-net-{}-many", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let exe = dir.join("many");
    let b = Command::new(env!("CARGO_BIN_EXE_slang")).arg("build").arg(&src).arg("-o").arg(&exe).arg("--leak-check").output().unwrap();
    assert!(b.status.success(), "build failed:\n{}", String::from_utf8_lossy(&b.stderr));
    for workers in ["1", "4"] {
        let mut child = Command::new(&exe)
            .arg(n.to_string())
            .env("SLANG_NO_BACKTRACE", "1")
            .env("SLANG_WORKERS", workers)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .unwrap();
        let start = Instant::now();
        while child.try_wait().unwrap().is_none() {
            if start.elapsed() > Duration::from_secs(120) {
                let _ = child.kill();
                panic!("{n} connections did not finish within 120 s (SLANG_WORKERS={workers})");
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        let out = child.wait_with_output().unwrap();
        assert!(out.status.success(), "{n} connections failed (SLANG_WORKERS={workers}):\n{}", String::from_utf8_lossy(&out.stderr));
        assert_eq!(String::from_utf8_lossy(&out.stdout), "echoed true connections\n");
    }
    let _ = std::fs::remove_dir_all(&dir);
}

// ---- HTTP/2 and WebSocket interop (compiler/tests/fixtures/h2) ----------------

fn have(tool: &str) -> bool {
    Command::new("sh").arg("-c").arg(format!("command -v {tool}")).output().map(|o| o.status.success()).unwrap_or(false)
}

/// Builds a fixture program (with `--leak-check`) into a fresh directory,
/// once per test process: the curl and Go tests both run `server`, in
/// parallel, and rebuilding it in place while the other test executes it
/// failed the spawn ("text file busy") under load.
fn build_fixture(name: &str) -> PathBuf {
    use std::collections::HashMap;
    use std::sync::{Mutex, OnceLock};
    static BUILT: OnceLock<Mutex<HashMap<String, PathBuf>>> = OnceLock::new();
    let mut built = BUILT.get_or_init(|| Mutex::new(HashMap::new())).lock().unwrap_or_else(|e| e.into_inner());
    if let Some(exe) = built.get(name) {
        return exe.clone();
    }
    ensure_runtime();
    let src = repo().join("compiler/tests/fixtures/h2").join(format!("{name}.sl"));
    let dir = std::env::temp_dir().join(format!("slang-h2-{}-{name}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let exe = dir.join(name);
    let b = Command::new(env!("CARGO_BIN_EXE_slang")).arg("build").arg(&src).arg("-o").arg(&exe).arg("--leak-check").output().unwrap();
    assert!(b.status.success(), "build of {name} failed:\n{}", String::from_utf8_lossy(&b.stderr));
    built.insert(name.to_string(), exe.clone());
    exe
}

/// The Slang fixture server, its h2c and TLS ports, and its stderr.
struct SlangServer {
    child: std::process::Child,
    h2c: u16,
    tls: u16,
    stderr: std::io::BufReader<std::process::ChildStderr>,
}

fn start_slang_server(workers: &str) -> SlangServer {
    use std::io::BufRead;
    let exe = build_fixture("server");
    let mut child = Command::new(&exe).env("SLANG_NO_BACKTRACE", "1").env("SLANG_WORKERS", workers).stdout(Stdio::null()).stderr(Stdio::piped()).spawn().unwrap();
    let mut stderr = std::io::BufReader::new(child.stderr.take().unwrap());
    let (mut h2c, mut tls) = (0, 0);
    for _ in 0..2 {
        let mut line = String::new();
        stderr.read_line(&mut line).unwrap();
        let mut it = line.split_whitespace();
        match (it.next(), it.next()) {
            (Some("h2c"), Some(p)) => h2c = p.parse().unwrap(),
            (Some("tls"), Some(p)) => tls = p.parse().unwrap(),
            _ => panic!("unexpected server output {line:?}"),
        }
    }
    SlangServer { child, h2c, tls, stderr }
}

impl SlangServer {
    /// GET /quit, then the server must exit 0 with nothing more on stderr
    /// (no leak report).
    fn stop(mut self) {
        use std::io::Read;
        let q = Command::new("curl").args(["-s", "--max-time", "10"]).arg(format!("http://127.0.0.1:{}/quit", self.h2c)).output().unwrap();
        assert_eq!(String::from_utf8_lossy(&q.stdout), "bye\n");
        let start = Instant::now();
        while self.child.try_wait().unwrap().is_none() {
            if start.elapsed() > Duration::from_secs(20) {
                let _ = self.child.kill();
                panic!("the fixture server did not stop");
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        let mut rest = String::new();
        self.stderr.read_to_string(&mut rest).unwrap();
        let status = self.child.wait().unwrap();
        assert!(status.success() && rest.is_empty(), "server exit {status:?}, stderr:\n{rest}");
    }
}

fn curl(args: &[&str]) -> String {
    let out = Command::new("curl").args(["-s", "--max-time", "20"]).args(args).output().unwrap();
    String::from_utf8_lossy(&out.stdout).into_owned()
}

/// curl (libcurl + nghttp2) against the Slang server: HTTP/2 with prior
/// knowledge (h2c), HTTP/2 over TLS via ALPN, HTTP/1.1 on both listeners,
/// large bodies in both directions (flow control), and the WebSocket
/// handshake (RFC 6455 sample key).
#[test]
fn curl_http2_and_websocket_against_slang_server() {
    if !have("curl") {
        eprintln!("skipping: no curl");
        return;
    }
    let v = Command::new("curl").arg("-V").output().unwrap();
    if !String::from_utf8_lossy(&v.stdout).contains("HTTP2") {
        eprintln!("skipping: curl without HTTP/2");
        return;
    }
    for workers in ["1", "4"] {
        let s = start_slang_server(workers);
        let h2c = format!("http://127.0.0.1:{}", s.h2c);
        let tls = format!("https://localhost:{}", s.tls);
        let head = curl(&["--http2-prior-knowledge", "-i", &format!("{h2c}/hello")]);
        assert!(head.starts_with("HTTP/2 200"), "h2c: {head}");
        assert!(head.contains("hello via HTTP/2.0"), "h2c: {head}");
        let head = curl(&["--http2", "-k", "-i", &format!("{tls}/hello")]);
        assert!(head.starts_with("HTTP/2 200") && head.contains("x-proto: HTTP/2.0"), "h2 over TLS: {head}");
        assert_eq!(curl(&["--http2-prior-knowledge", &format!("{h2c}/big")]).len(), 1_000_000);
        assert_eq!(curl(&["--http2", "-k", &format!("{tls}/big")]).len(), 1_000_000);
        let file = std::env::temp_dir().join(format!("slang-h2-{}-body", std::process::id()));
        std::fs::write(&file, "z".repeat(300_000)).unwrap();
        let body = format!("@{}", file.display());
        assert_eq!(curl(&["--http2-prior-knowledge", "--data-binary", &body, &format!("{h2c}/echo")]), "300000 bytes\n");
        assert_eq!(curl(&["--http2", "-k", "--data-binary", &body, &format!("{tls}/echo")]), "300000 bytes\n");
        let _ = std::fs::remove_file(&file);
        assert_eq!(curl(&[&format!("{h2c}/hello")]), "hello via HTTP/1.1\n");
        assert_eq!(curl(&["--http1.1", "-k", &format!("{tls}/hello")]), "hello via HTTP/1.1\n");
        // several streams on one connection
        let many = curl(&["--http2-prior-knowledge", "-Z", &format!("{h2c}/hello"), &format!("{h2c}/hello"), &format!("{h2c}/hello")]);
        assert_eq!(many, "hello via HTTP/2.0\n".repeat(3));
        let ws = curl(&["-i", "-N", "--max-time", "2", "-H", "Connection: Upgrade", "-H", "Upgrade: websocket", "-H", "Sec-WebSocket-Version: 13", "-H", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==", &format!("{h2c}/ws")]);
        assert!(ws.starts_with("HTTP/1.1 101 Switching Protocols\r\n"), "ws: {ws}");
        assert!(ws.contains("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"), "ws: {ws}");
        s.stop();
    }
}

/// Go's net/http (stdlib only, `fixtures/h2/main.go`): its HTTP/2 client
/// (h2c via http.Protocols, TLS via ALPN) against the Slang server, the
/// Slang client against its HTTP/2 server, and WebSockets both ways
/// (framing written by hand on the Go side).
#[test]
fn go_http2_and_websocket_interop() {
    if !have("go") || !have("curl") {
        eprintln!("skipping: no go or curl");
        return;
    }
    let dir = std::env::temp_dir().join(format!("slang-h2-{}-go", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let gobin = dir.join("goh2");
    let b = Command::new("go").arg("build").arg("-o").arg(&gobin).arg("main.go").current_dir(repo().join("compiler/tests/fixtures/h2")).env("GOFLAGS", "-mod=mod").env("GO111MODULE", "off").output().unwrap();
    if !b.status.success() {
        eprintln!("skipping: go build failed:\n{}", String::from_utf8_lossy(&b.stderr));
        return;
    }
    // Go client -> Slang server
    let s = start_slang_server("4");
    let out = Command::new(&gobin).arg("client").arg(format!("http://127.0.0.1:{}", s.h2c)).arg(format!("https://127.0.0.1:{}", s.tls)).output().unwrap();
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    let want_one = "GET HTTP/2.0 200 text/plain; charset=utf-8 \"hello via HTTP/2.0\\n\"\nGET HTTP/2.0 200 text/plain; charset=utf-8 \"1000000 bytes\"\nPOST HTTP/2.0 200 text/plain; charset=utf-8 \"100000 bytes\\n\"\nGET HTTP/2.0 404 text/plain; charset=utf-8 \"404 Not Found\\n\"\n";
    assert_eq!(text, want_one.repeat(2), "go client: {}", String::from_utf8_lossy(&out.stderr));
    let out = Command::new(&gobin).arg("wsclient").arg(format!("ws://127.0.0.1:{}/ws", s.h2c)).output().unwrap();
    assert_eq!(String::from_utf8_lossy(&out.stdout), "handshake: 101 true\necho: 1 hello from go\nbinary echo: 2 70000 true\npong: 10 ping\nclose: 8 1000\n");
    s.stop();

    // Slang client -> Go server
    use std::io::BufRead;
    let mut go = Command::new(&gobin).arg("server").stdout(Stdio::piped()).stderr(Stdio::null()).spawn().unwrap();
    let mut rd = std::io::BufReader::new(go.stdout.take().unwrap());
    let (mut tls, mut h2c) = (0u16, 0u16);
    for _ in 0..2 {
        let mut line = String::new();
        rd.read_line(&mut line).unwrap();
        let mut it = line.split_whitespace();
        match (it.next(), it.next()) {
            (Some("tls"), Some(p)) => tls = p.parse().unwrap(),
            (Some("h2c"), Some(p)) => h2c = p.parse().unwrap(),
            _ => panic!("unexpected go server output {line:?}"),
        }
    }
    let cli = build_fixture("client");
    for workers in ["1", "4"] {
        let out = Command::new(&cli).arg(format!("http://127.0.0.1:{h2c}")).arg(format!("https://127.0.0.1:{tls}")).env("SLANG_NO_BACKTRACE", "1").env("SLANG_WORKERS", workers).output().unwrap();
        let one = "HTTP/2.0 200 HTTP/2.0 hello from go via HTTP/2.0\n10 x /big: 5000000\nPOST 150000 bytes\nchunk 0 | chunk 1 | chunk 2\ngzip: 10000 true true\nstreamed: 500000\n";
        assert_eq!(String::from_utf8_lossy(&out.stdout), format!("{}{}dials: 1 1\n", one, one), "slang client (SLANG_WORKERS={workers}): {}", String::from_utf8_lossy(&out.stderr));
        assert!(out.status.success() && out.stderr.is_empty(), "slang client stderr: {}", String::from_utf8_lossy(&out.stderr));
        let out = Command::new(&cli).arg("ws").arg(format!("ws://127.0.0.1:{h2c}/ws")).env("SLANG_WORKERS", workers).output().unwrap();
        assert_eq!(String::from_utf8_lossy(&out.stdout), "echo: 1 hello from slang\nfragments: abc\nbinary: 2 70000\nclosed\n");
        assert!(out.status.success() && out.stderr.is_empty(), "ws client stderr: {}", String::from_utf8_lossy(&out.stderr));
    }
    let _ = curl(&[&format!("http://127.0.0.1:{h2c}/quit")]);
    let _ = go.wait();
    let _ = std::fs::remove_dir_all(&dir);
}
