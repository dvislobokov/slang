//! Every ```` ```zell ```` block of `docs/guide/*.md` and `README.md` is
//! a complete program that must compile; a block ending with an
//! `// Output:` comment is also run and its standard output compared
//! (white space around the output trimmed, as for `zell test` examples);
//! a block with a `// check-only` line is only type-checked (servers,
//! programs reading files or the network, `@test` files); a block whose
//! first line is `// error: <text>` must fail to compile with a diagnostic
//! containing `<text>` (what the compiler rejects); a block with a
//! `// zell test` line is a test file that `zell test` must pass. A block with none of
//! these markers fails the test, so no example goes stale silently.
//!
//! Needs `clang` and `runtime/libzell_rt.a` (built with `make -C runtime`).

use std::path::{Path, PathBuf};
use std::process::Command;

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn ensure_runtime() {
    if !repo().join("runtime/libzell_rt.a").is_file() {
        let st = Command::new("make").arg("-C").arg(repo().join("runtime")).status().expect("cannot run make");
        assert!(st.success(), "make -C runtime failed");
    }
}

/// One ```zell block: (file, line of the fence, source).
struct Block {
    file: String,
    line: usize,
    src: String,
}

fn blocks() -> Vec<Block> {
    let mut files: Vec<PathBuf> = std::fs::read_dir(repo().join("docs/guide")).unwrap().map(|e| e.unwrap().path()).filter(|p| p.extension().is_some_and(|x| x == "md")).collect();
    files.sort();
    files.push(repo().join("README.md"));
    let mut out = Vec::new();
    for f in files {
        let text = std::fs::read_to_string(&f).unwrap();
        let name = f.strip_prefix(repo()).unwrap().display().to_string();
        let mut cur: Option<(usize, Vec<&str>)> = None;
        for (i, l) in text.lines().enumerate() {
            match &mut cur {
                None if l.trim_start().starts_with("```zell") => cur = Some((i + 1, Vec::new())),
                Some((start, body)) if l.trim_start().starts_with("```") => {
                    out.push(Block { file: name.clone(), line: *start, src: body.join("\n") + "\n" });
                    cur = None;
                }
                Some((_, body)) => body.push(l),
                None => {}
            }
        }
        assert!(cur.is_none(), "{name}: unterminated code block");
    }
    out
}

/// The expected output of a block: the comment lines after its last
/// `// Output:` line, if they end the block.
fn expected_output(src: &str) -> Option<String> {
    let lines: Vec<&str> = src.trim_end().lines().collect();
    let at = lines.iter().rposition(|l| l.trim() == "// Output:")?;
    let rest = &lines[at + 1..];
    if !rest.iter().all(|l| l.trim_start().starts_with("//")) {
        return None;
    }
    let body: Vec<&str> = rest.iter().map(|l| {
        let l = l.trim_start().strip_prefix("//").unwrap();
        l.strip_prefix(' ').unwrap_or(l)
    }).collect();
    Some(body.join("\n").trim().to_string())
}

fn normalize(s: &str) -> String {
    s.trim().lines().map(str::trim_end).collect::<Vec<_>>().join("\n")
}

/// Checks one block; `Err` describes the failure.
fn check(b: &Block, dir: &Path) -> Result<(), String> {
    let want = expected_output(&b.src);
    let check_only = b.src.lines().any(|l| l.trim() == "// check-only");
    let want_error = b.src.lines().next().and_then(|l| l.strip_prefix("// error: ")).map(str::trim);
    let run_tests = b.src.lines().any(|l| l.trim() == "// zell test");
    if want.is_none() && !check_only && want_error.is_none() && !run_tests {
        return Err("needs a trailing `// Output:` comment, a `// check-only` line or a leading `// error:` line".into());
    }
    // test files are named like test files, so `zell test` finds their examples
    let src = dir.join(format!("block{}{}.zl", b.line, if run_tests { "_test" } else { "" }));
    std::fs::write(&src, &b.src).unwrap();
    let zell = env!("CARGO_BIN_EXE_zell");
    if let Some(msg) = want_error {
        let o = Command::new(zell).arg("check").arg(&src).output().unwrap();
        let err = String::from_utf8_lossy(&o.stderr);
        return if !o.status.success() && err.contains(msg) { Ok(()) } else { Err(format!("expected a compile error containing {msg:?}, got status {:?}:\n{err}", o.status.code())) };
    }
    if run_tests {
        let o = Command::new(zell).arg("test").arg(&src).env("ZELL_NO_BACKTRACE", "1").output().unwrap();
        let out = String::from_utf8_lossy(&o.stdout);
        return if o.status.success() && out.contains("ok") { Ok(()) } else { Err(format!("zell test failed:\n{out}{}", String::from_utf8_lossy(&o.stderr))) };
    }
    if check_only {
        let o = Command::new(zell).arg("check").arg(&src).output().unwrap();
        return if o.status.success() { Ok(()) } else { Err(format!("zell check failed:\n{}", String::from_utf8_lossy(&o.stderr))) };
    }
    let exe = dir.join(format!("block{}", b.line));
    let o = Command::new(zell).arg("build").arg(&src).arg("-o").arg(&exe).output().unwrap();
    if !o.status.success() {
        return Err(format!("zell build failed:\n{}", String::from_utf8_lossy(&o.stderr)));
    }
    let r = Command::new(&exe).current_dir(dir).env("ZELL_NO_BACKTRACE", "1").output().unwrap();
    let got = String::from_utf8_lossy(&r.stdout).to_string();
    let want = want.unwrap();
    if normalize(&got) != normalize(&want) {
        return Err(format!("output differs\n--- got:\n{got}\n--- want:\n{want}\n--- stderr:\n{}", String::from_utf8_lossy(&r.stderr)));
    }
    Ok(())
}

#[test]
fn guide_code_blocks_compile_and_run() {
    ensure_runtime();
    let all = blocks();
    assert!(all.len() >= 40, "only {} zell blocks found", all.len());
    let filter = std::env::var("GUIDE_FILTER").unwrap_or_default();
    let all: Vec<Block> = all.into_iter().filter(|b| format!("{}:{}", b.file, b.line).contains(&filter)).collect();
    let dir = std::env::temp_dir().join(format!("zell-guide-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let failures = std::sync::Mutex::new(Vec::new());
    let next = std::sync::atomic::AtomicUsize::new(0);
    std::thread::scope(|s| {
        for _ in 0..4 {
            s.spawn(|| loop {
                let i = next.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                let Some(b) = all.get(i) else { break };
                if let Err(e) = check(b, &dir) {
                    failures.lock().unwrap().push(format!("{}:{}: {e}", b.file, b.line));
                }
            });
        }
    });
    let _ = std::fs::remove_dir_all(&dir);
    let failures = failures.into_inner().unwrap();
    assert!(failures.is_empty(), "{} of {} guide blocks failed:\n\n{}", failures.len(), all.len(), failures.join("\n\n"));
}
