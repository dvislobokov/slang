//! `slang fmt`: golden cases (`tests/fixtures/fmt/NN_name.in.sl` ->
//! `NN_name.out.sl`) plus idempotence and round-trip over every `.sl` file
//! of the repository's examples, benchmarks and spec corpus.

use slang::fmt::{format_source, normalized, FmtError};
use std::path::{Path, PathBuf};

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn sl_files(dir: &Path, out: &mut Vec<PathBuf>) {
    for e in std::fs::read_dir(dir).unwrap().flatten() {
        let p = e.path();
        if p.is_dir() {
            sl_files(&p, out);
        } else if p.extension().is_some_and(|x| x == "sl") {
            out.push(p);
        }
    }
}

fn parse(src: &str) -> slang::ast::File {
    let (file, diags) = slang::parser::parse_source(src);
    assert!(!diags.has_errors(), "{:?}", diags.items);
    file
}

#[test]
fn golden_cases() {
    let dir = repo().join("compiler/tests/fixtures/fmt");
    let mut inputs: Vec<PathBuf> = std::fs::read_dir(&dir).unwrap().flatten().map(|e| e.path()).filter(|p| p.to_string_lossy().ends_with(".in.sl")).collect();
    inputs.sort();
    assert!(inputs.len() >= 10, "expected at least 10 golden cases, found {}", inputs.len());
    let mut failures = Vec::new();
    for input in &inputs {
        let src = std::fs::read_to_string(input).unwrap();
        let expected_path = PathBuf::from(input.to_string_lossy().replace(".in.sl", ".out.sl"));
        let expected = std::fs::read_to_string(&expected_path).unwrap_or_else(|_| panic!("missing {}", expected_path.display()));
        match format_source(&src) {
            Ok(out) if out == expected => {}
            Ok(out) => failures.push(format!("{}:\n--- expected\n{expected}--- got\n{out}", input.display())),
            Err(e) => failures.push(format!("{}: {e}", input.display())),
        }
        // The expected output is a fixed point.
        assert_eq!(format_source(&expected).unwrap(), expected, "{} is not stable", expected_path.display());
    }
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}

#[test]
fn corpus_is_idempotent_and_preserves_the_syntax_tree() {
    let mut files = Vec::new();
    for d in ["examples", "benchmarks", "tests/spec"] {
        sl_files(&repo().join(d), &mut files);
    }
    files.sort();
    assert!(files.len() > 100);
    let mut checked = 0;
    let mut failures = Vec::new();
    for f in &files {
        let src = std::fs::read_to_string(f).unwrap();
        let once = match format_source(&src) {
            Ok(o) => o,
            // Deliberately broken programs (lexer/parser error tests).
            Err(FmtError::Syntax(_)) => continue,
            Err(e) => {
                failures.push(format!("{}: {e}", f.display()));
                continue;
            }
        };
        checked += 1;
        let twice = format_source(&once).unwrap();
        if twice != once {
            failures.push(format!("{}: not idempotent", f.display()));
        }
        if normalized(&parse(&src)) != normalized(&parse(&once)) {
            failures.push(format!("{}: syntax tree changed", f.display()));
        }
        let lines = |s: &str| s.matches("//").count();
        if lines(&src) != lines(&once) {
            failures.push(format!("{}: comment count changed", f.display()));
        }
    }
    assert!(checked > 100, "only {checked} files formatted");
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}

#[test]
fn syntax_errors_are_reported_not_formatted() {
    match format_source("namespace t\nfunc f( {\n") {
        Err(FmtError::Syntax(d)) => assert!(d.has_errors()),
        other => panic!("{other:?}"),
    }
}

#[test]
fn long_lines_break_and_short_ones_join() {
    let src = "namespace t\n\nfunc f() {\n    g(\n        1,\n        2,\n    )\n}\n";
    assert_eq!(format_source(src).unwrap(), "namespace t\n\nfunc f() {\n    g(1, 2)\n}\n");
    let long = format!("namespace t\n\nfunc f() {{\n    g({})\n}}\n", (0..30).map(|i| format!("arg{i}")).collect::<Vec<_>>().join(", "));
    let out = format_source(&long).unwrap();
    assert!(out.lines().all(|l| l.chars().count() <= 100), "{out}");
    assert!(out.contains("        arg29,\n    )"), "{out}");
}

#[test]
fn cli_check_and_stdin() {
    let dir = std::env::temp_dir().join(format!("slang-fmt-cli-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let file = dir.join("a.sl");
    std::fs::write(&file, "namespace t\nfunc f() { var x=1\n_ = x }\n").unwrap();
    let slang = env!("CARGO_BIN_EXE_slang");
    let check = std::process::Command::new(slang).args(["fmt", "--check"]).arg(&dir).output().unwrap();
    assert_eq!(check.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&check.stdout).contains("a.sl"));
    let diff = std::process::Command::new(slang).args(["fmt", "--diff"]).arg(&file).output().unwrap();
    assert!(String::from_utf8_lossy(&diff.stdout).contains("+    var x = 1"), "{}", String::from_utf8_lossy(&diff.stdout));
    let write = std::process::Command::new(slang).arg("fmt").arg(&dir).output().unwrap();
    assert!(write.status.success());
    assert_eq!(std::fs::read_to_string(&file).unwrap(), "namespace t\n\nfunc f() {\n    var x = 1\n    _ = x\n}\n");
    let again = std::process::Command::new(slang).args(["fmt", "--check"]).arg(&dir).output().unwrap();
    assert!(again.status.success());
    use std::io::Write;
    let mut child = std::process::Command::new(slang)
        .args(["fmt", "--stdin"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    child.stdin.take().unwrap().write_all(b"namespace t\nvar x=[1,2]\n").unwrap();
    let out = child.wait_with_output().unwrap();
    assert_eq!(String::from_utf8_lossy(&out.stdout), "namespace t\n\nvar x = [1, 2]\n");
    let _ = std::fs::remove_dir_all(&dir);
}
