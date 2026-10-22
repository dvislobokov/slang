//! Running the `zell` CLI for the tools that build code (`run_tests`,
//! `build`, `run`): finding the binary, a timeout, captured output.

use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

/// Output kept from each stream of a command (the tail is kept).
const MAX_OUTPUT: usize = 24_000;

/// The `zell` executable: `$ZELL_BIN`, else `zell` next to this server
/// (an installed toolchain or a cargo target directory), else `zell` on
/// `PATH`.
pub fn zell_bin() -> PathBuf {
    if let Some(b) = std::env::var_os("ZELL_BIN").filter(|v| !v.is_empty()) {
        return PathBuf::from(b);
    }
    let exe = format!("zell{}", std::env::consts::EXE_SUFFIX);
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)) {
        let sibling = dir.join(&exe);
        if sibling.is_file() {
            return sibling;
        }
        // cargo test binaries live in target/<profile>/deps
        if let Some(up) = dir.parent().map(|d| d.join(&exe)).filter(|p| dir.ends_with("deps") && p.is_file()) {
            return up;
        }
    }
    PathBuf::from(exe)
}

/// What a command did.
pub struct Output {
    /// `None` when it was killed (timeout) or did not start.
    pub code: Option<i32>,
    pub stdout: String,
    pub stderr: String,
    pub timed_out: bool,
}

fn tail(mut s: String) -> String {
    if s.len() > MAX_OUTPUT {
        let mut cut = s.len() - MAX_OUTPUT;
        while !s.is_char_boundary(cut) {
            cut += 1;
        }
        s = format!("[... {cut} bytes cut ...]\n{}", &s[cut..]);
    }
    s
}

/// Runs `zell <args>` in `dir` with stdin closed, killing it after `timeout`.
pub fn zell(dir: &Path, args: &[String], timeout: Duration) -> Result<Output, String> {
    let bin = zell_bin();
    let mut child = Command::new(&bin)
        .args(args)
        .current_dir(dir)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .env("NO_COLOR", "1")
        .spawn()
        .map_err(|e| format!("cannot run {}: {e} (install the Zell toolchain or set ZELL_BIN)", bin.display()))?;
    // Readers report through channels: a program `zell run` started may
    // outlive a killed `zell` and keep the pipes open.
    let reader = |r: Option<Box<dyn Read + Send>>| {
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            let mut buf = Vec::new();
            if let Some(mut r) = r {
                let _ = r.read_to_end(&mut buf);
            }
            let _ = tx.send(String::from_utf8_lossy(&buf).into_owned());
        });
        rx
    };
    let out = reader(child.stdout.take().map(|s| Box::new(s) as Box<dyn Read + Send>));
    let err = reader(child.stderr.take().map(|s| Box::new(s) as Box<dyn Read + Send>));
    let deadline = Instant::now() + timeout;
    let (code, timed_out) = loop {
        match child.try_wait() {
            Ok(Some(status)) => break (status.code(), false),
            Ok(None) if Instant::now() >= deadline => {
                let _ = child.kill();
                let _ = child.wait();
                break (None, true);
            }
            Ok(None) => std::thread::sleep(Duration::from_millis(20)),
            Err(e) => return Err(format!("waiting for zell: {e}")),
        }
    };
    let grace = Duration::from_secs(if timed_out { 1 } else { 10 });
    let stdout = tail(out.recv_timeout(grace).unwrap_or_default());
    let stderr = tail(err.recv_timeout(grace).unwrap_or_default());
    Ok(Output { code, stdout, stderr, timed_out })
}

// -- tools -------------------------------------------------------------------

use crate::tools::{bool_arg, int_arg, str_arg, ToolResult};
use crate::workspace::Workspace;
use serde_json::Value;
use std::fmt::Write as _;

fn path_arg(ws: &Workspace, args: &Value, out: &mut Vec<String>) -> Result<(), String> {
    if let Some(p) = str_arg(args, "path") {
        let abs = ws.abs(p);
        if !abs.exists() {
            return Err(format!("no such file or directory: {p}"));
        }
        out.push(abs.display().to_string());
    }
    Ok(())
}

fn status(o: &Output, timeout: Duration) -> String {
    match o.code {
        _ if o.timed_out => format!("killed after {}s (timeout)", timeout.as_secs()),
        Some(c) => format!("exit code {c}"),
        None => "killed by a signal".into(),
    }
}

fn streams(out: &mut String, o: &Output) {
    for (name, s) in [("stdout", &o.stdout), ("stderr", &o.stderr)] {
        if !s.trim().is_empty() {
            let _ = write!(out, "--- {name} ---\n{}", s);
            if !s.ends_with('\n') {
                out.push('\n');
            }
        }
    }
}

/// `run_tests`: `zell test --json`, summarised.
pub fn run_tests(ws: &mut Workspace, args: &Value) -> ToolResult {
    let mut cmd = vec!["test".to_string()];
    path_arg(ws, args, &mut cmd)?;
    cmd.push("--json".into());
    if let Some(f) = str_arg(args, "filter") {
        cmd.push("--filter".into());
        cmd.push(f.into());
    }
    let timeout = Duration::from_secs(int_arg(args, "timeout_sec").unwrap_or(300) as u64);
    let o = zell(&ws.root, &cmd, timeout)?;
    let (mut passed, mut failed, mut skipped) = (Vec::new(), Vec::new(), Vec::new());
    let mut outputs: std::collections::HashMap<String, String> = std::collections::HashMap::new();
    let mut other = String::new();
    for line in o.stdout.lines() {
        let Ok(ev) = serde_json::from_str::<Value>(line) else {
            other.push_str(line);
            other.push('\n');
            continue;
        };
        let action = ev.get("Action").and_then(Value::as_str).unwrap_or("");
        let Some(test) = ev.get("Test").and_then(Value::as_str) else { continue };
        let elapsed = ev.get("Elapsed").and_then(Value::as_f64).unwrap_or(0.0);
        match action {
            "output" => outputs.entry(test.to_string()).or_default().push_str(ev.get("Output").and_then(Value::as_str).unwrap_or("")),
            "pass" => passed.push(test.to_string()),
            "fail" => failed.push((test.to_string(), elapsed)),
            "skip" => skipped.push(test.to_string()),
            _ => {}
        }
    }
    let total = passed.len() + failed.len() + skipped.len();
    let mut out = String::new();
    if total == 0 {
        let _ = writeln!(out, "No tests ran ({}).", status(&o, timeout));
        if !other.trim().is_empty() {
            let _ = write!(out, "--- stdout ---\n{other}");
        }
        if !o.stderr.trim().is_empty() {
            let _ = write!(out, "--- stderr ---\n{}", o.stderr);
        }
        return if o.code == Some(0) { Ok(out) } else { Err(out) };
    }
    let word = if failed.is_empty() && o.code == Some(0) { "ok" } else { "FAIL" };
    let _ = writeln!(out, "{word}: {} passed, {} failed, {} skipped", passed.len(), failed.len(), skipped.len());
    if o.timed_out {
        let _ = writeln!(out, "(killed after {}s: the run did not finish)", timeout.as_secs());
    }
    for (t, secs) in &failed {
        let _ = writeln!(out, "\n--- FAIL: {t} ({secs:.2}s)");
        if let Some(text) = outputs.get(t) {
            out.push_str(text);
            if !text.ends_with('\n') {
                out.push('\n');
            }
        }
    }
    if !skipped.is_empty() {
        let _ = writeln!(out, "\nskipped: {}", skipped.join(", "));
    }
    if !failed.is_empty() && !o.stderr.trim().is_empty() {
        let _ = write!(out, "--- stderr ---\n{}", o.stderr);
    }
    Ok(out)
}

/// `build`: `zell build [path] [--release]`.
pub fn build(ws: &mut Workspace, args: &Value) -> ToolResult {
    let mut cmd = vec!["build".to_string()];
    path_arg(ws, args, &mut cmd)?;
    if bool_arg(args, "release", false) {
        cmd.push("--release".into());
    }
    let timeout = Duration::from_secs(600);
    let o = zell(&ws.root, &cmd, timeout)?;
    let mut out = if o.code == Some(0) { "Build succeeded.\n".to_string() } else { format!("Build failed ({}).\n", status(&o, timeout)) };
    streams(&mut out, &o);
    if o.code == Some(0) {
        Ok(out)
    } else {
        Err(out)
    }
}

/// `run`: `zell run [path] -- args`, with a timeout.
pub fn run(ws: &mut Workspace, args: &Value) -> ToolResult {
    let mut cmd = vec!["run".to_string()];
    path_arg(ws, args, &mut cmd)?;
    if bool_arg(args, "leak_check", false) {
        cmd.push("--leak-check".into());
    }
    let extra: Vec<String> = args.get("args").and_then(Value::as_array).map(|v| v.iter().filter_map(|a| a.as_str().map(str::to_string)).collect()).unwrap_or_default();
    if !extra.is_empty() {
        cmd.push("--".into());
        cmd.extend(extra);
    }
    let timeout = Duration::from_secs(int_arg(args, "timeout_sec").unwrap_or(60) as u64);
    let o = zell(&ws.root, &cmd, timeout)?;
    let mut out = format!("{}\n", status(&o, timeout));
    streams(&mut out, &o);
    if o.stdout.trim().is_empty() && o.stderr.trim().is_empty() {
        out.push_str("(no output)\n");
    }
    // A program's non-zero exit is a result, not a tool failure; a timeout is.
    if o.timed_out {
        Err(out)
    } else {
        Ok(out)
    }
}
