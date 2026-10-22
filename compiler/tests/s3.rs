//! `cloud.s3` / `cloud.blob` against a real MinIO server.
//!
//! The test starts `minio server` (from `PATH` or `$MINIO_BIN`) on a free
//! port with a temporary data directory and root credentials, waits for
//! `/minio/health/live`, runs `tests/fixtures/s3/s3_real.zl` (built with
//! `--leak-check`) and compares its output with `s3_real.out`. Without a
//! MinIO binary the test is skipped with a note (MinIO is not packaged for
//! apt; `go install github.com/minio/minio@latest` builds it); the client
//! is still covered by the spec tests `tests/spec/stdlib5/s3_*.zl` (AWS
//! SigV4 vectors and an in-process S3 server that verifies signatures).

use std::net::{TcpListener, TcpStream};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn minio_bin() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("MINIO_BIN") {
        return Some(PathBuf::from(p));
    }
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path).map(|d| d.join("minio")).find(|p| p.is_file())
}

struct Server(Child);

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

fn live(port: u16) -> bool {
    let Ok(mut s) = TcpStream::connect(("127.0.0.1", port)) else { return false };
    let _ = s.set_read_timeout(Some(Duration::from_secs(2)));
    if s.write_all(b"GET /minio/health/live HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n").is_err() {
        return false;
    }
    let mut buf = String::new();
    let _ = s.read_to_string(&mut buf);
    buf.starts_with("HTTP/1.1 200")
}

#[test]
fn s3_real_minio() {
    let Some(bin) = minio_bin() else {
        eprintln!("skipping: minio not found (set MINIO_BIN or put it on PATH)");
        return;
    };
    let dir = std::env::temp_dir().join(format!("zell_s3test_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(dir.join("data")).unwrap();
    let port = TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
    let console = TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
    let child = Command::new(&bin)
        .arg("server")
        .arg(dir.join("data"))
        .arg("--address")
        .arg(format!("127.0.0.1:{port}"))
        .arg("--console-address")
        .arg(format!("127.0.0.1:{console}"))
        .env("MINIO_ROOT_USER", "zelladmin")
        .env("MINIO_ROOT_PASSWORD", "zellsecret123")
        .env("MINIO_BROWSER", "off")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .expect("cannot start minio");
    let server = Server(child);
    let start = Instant::now();
    while !live(port) {
        assert!(start.elapsed() < Duration::from_secs(30), "minio did not start");
        std::thread::sleep(Duration::from_millis(100));
    }
    if !repo().join("runtime/libzell_rt.a").is_file() {
        assert!(Command::new("make").arg("-C").arg(repo().join("runtime")).status().unwrap().success());
    }
    let src = repo().join("compiler/tests/fixtures/s3/s3_real.zl");
    let exe = dir.join("s3_real");
    let b = Command::new(env!("CARGO_BIN_EXE_zell")).arg("build").arg(&src).arg("-o").arg(&exe).arg("--leak-check").output().unwrap();
    assert!(b.status.success(), "build failed:\n{}", String::from_utf8_lossy(&b.stderr));
    let run = Command::new(&exe)
        .env("S3_ENDPOINT", format!("http://127.0.0.1:{port}"))
        .env("S3_ACCESS_KEY", "zelladmin")
        .env("S3_SECRET_KEY", "zellsecret123")
        .env("ZELL_NO_BACKTRACE", "1")
        .output()
        .unwrap();
    let stdout = String::from_utf8_lossy(&run.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&run.stderr).into_owned();
    assert!(run.status.success(), "program failed ({:?}):\n{stdout}\n{stderr}", run.status.code());
    let want = std::fs::read_to_string(repo().join("compiler/tests/fixtures/s3/s3_real.out")).unwrap();
    assert_eq!(stdout, want, "output differs; stderr:\n{stderr}");
    drop(server);
    let _ = std::fs::remove_dir_all(&dir);
}
