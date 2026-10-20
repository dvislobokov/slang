//! End-to-end tests of the async runtime pieces that talk to the network:
//! `examples/http_server.sl` (an HTTP server and client in one program),
//! `examples/tcp_echo.sl`, `tests/spec/stdlib/net_many_conns.sl` (up to
//! 10 000 concurrent connections) and
//! `examples/channels.sl`, `cancel.sl` and `task_context.sl` are built with `--leak-check` and run with a
//! deadline, with several worker counts, and must print their `.out` file
//! byte for byte and release every object (sockets, tasks, channels).
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
