//! `slang test` end to end on `tests/fixtures/testing` (std.testing).

use std::path::{Path, PathBuf};
use std::process::Command;

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn slang_test(args: &[&str]) -> (i32, String) {
    let lib = repo().join("runtime/libslang_rt.a");
    if !lib.is_file() {
        let st = Command::new("make").arg("-C").arg(repo().join("runtime")).status().expect("cannot run make");
        assert!(st.success(), "make -C runtime failed");
    }
    let out = Command::new(env!("CARGO_BIN_EXE_slang"))
        .arg("test")
        .arg("compiler/tests/fixtures/testing")
        .args(args)
        .current_dir(repo())
        .env("SLANG_NO_BACKTRACE", "1")
        .output()
        .expect("cannot run slang");
    (out.status.code().unwrap_or(-1), String::from_utf8_lossy(&out.stdout).to_string())
}

#[test]
fn reports_failures_and_summary() {
    let (code, out) = slang_test(&[]);
    assert_eq!(code, 1, "{out}");
    for want in [
        "    about to fail\n    got 4, want 5\n    got \"a\", want \"b\"\n--- FAIL: TestFailing (",
        "    stop here\n--- FAIL: TestFatalStops (",
        "    (exit status 2)\n--- FAIL: TestPanics (",
        "FAIL\nFAIL\t3 passed, 3 failed, 1 skipped (",
    ] {
        assert!(out.contains(want), "missing {want:?} in:\n{out}");
    }
    assert!(!out.contains("never printed"), "{out}");
    assert!(!out.contains("TestAdd"), "passing tests are quiet without -v:\n{out}");
}

#[test]
fn filter_verbose_and_subtests() {
    let (code, out) = slang_test(&["-v", "--filter", "^TestAdd$|plain"]);
    assert_eq!(code, 0, "{out}");
    assert!(out.contains("=== RUN   TestAdd\n    === RUN   TestAdd/1+2\n    --- PASS: TestAdd/1+2 ("), "{out}");
    assert!(out.contains("--- PASS: plainTest ("), "{out}");
    assert!(out.contains("ok  \t2 passed, 0 failed, 0 skipped ("), "{out}");
}

#[test]
fn json_events() {
    let (code, out) = slang_test(&["--json", "--filter", "TestDiv|TestSkipped"]);
    assert_eq!(code, 0, "{out}");
    assert!(out.contains("{\"Action\":\"run\",\"Test\":\"TestDiv\"}\n{\"Action\":\"pass\",\"Test\":\"TestDiv\",\"Elapsed\":"), "{out}");
    assert!(out.contains("{\"Action\":\"skip\",\"Test\":\"TestSkipped\""), "{out}");
}

#[test]
fn benchmarks() {
    let (code, out) = slang_test(&["--bench", "--filter", "Bench", "--benchtime", "20"]);
    assert_eq!(code, 0, "{out}");
    assert!(out.contains("BenchAdd ") && out.contains(" ns/op ") && out.contains(" allocs/op"), "{out}");
}
