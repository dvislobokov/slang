//! Cross-compilation tests (docs/PLATFORMS.md): `slang build --target
//! windows-amd64` on Linux, the executables run under Wine and compared
//! byte for byte with the examples' `.out` files; `--target macos-arm64
//! --emit=obj` (no macOS SDK here, so no linking); the C runtime's unit
//! tests under Wine.
//!
//! Needs clang with the mingw-w64 sysroot (`apt-get install mingw-w64`),
//! `wine` and the cross-built runtime (`make -C runtime
//! CROSS=x86_64-w64-mingw32`, built by the tests when missing). Without
//! the toolchain or Wine a test prints why and passes (CI's Linux cross job
//! installs both).
//!
//! Output: Slang's `println` writes `\n` on every platform and the runtime
//! puts stdout/stderr in binary mode, so a Windows executable's output is
//! compared as is, with no CRLF normalisation. Wine runs with
//! `LC_ALL=C.UTF-8` (it maps UTF-16 file names to the Unix locale's
//! charset, so non-ASCII names need a UTF-8 locale) and `WINEDEBUG=-all`.

use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::OnceLock;

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn slang() -> &'static str {
    env!("CARGO_BIN_EXE_slang")
}

fn tmpdir(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("slang-cross-{}-{name}", std::process::id()));
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn ok(cmd: &str, args: &[&str]) -> bool {
    Command::new(cmd).args(args).output().is_ok_and(|o| o.status.success())
}

/// Why the Windows cross tests cannot run here, or None if they can (the
/// cross runtime is built on first use).
fn windows_unavailable() -> Option<String> {
    static STATE: OnceLock<Option<String>> = OnceLock::new();
    STATE
        .get_or_init(|| {
            if !Path::new("/usr/x86_64-w64-mingw32/include/windows.h").is_file() && !ok("x86_64-w64-mingw32-gcc", &["--version"]) {
                return Some("mingw-w64 is not installed".into());
            }
            if !ok("clang", &["--version"]) && !ok("clang-18", &["--version"]) {
                return Some("clang is not installed".into());
            }
            if !ok("wine", &["--version"]) {
                return Some("wine is not installed".into());
            }
            let lib = repo().join("runtime/build/x86_64-pc-windows-gnu/libslang_rt.a");
            let st = Command::new("make").arg("-C").arg(repo().join("runtime")).arg("CROSS=x86_64-w64-mingw32").arg("-j8").output().expect("cannot run make");
            if !st.status.success() || !lib.is_file() {
                return Some(format!("make -C runtime CROSS=x86_64-w64-mingw32 failed:\n{}", String::from_utf8_lossy(&st.stderr)));
            }
            // A fresh Wine prefix has no %TEMP% directory yet.
            let _ = wine().args(["cmd", "/c", "if not exist %TEMP% mkdir %TEMP%"]).output();
            None
        })
        .clone()
}

fn wine() -> Command {
    let mut c = Command::new("wine");
    c.env("WINEDEBUG", "-all").env("LC_ALL", "C.UTF-8").env("SLANG_NO_BACKTRACE", "1");
    c
}

fn build(src: &Path, out: &Path, extra: &[&str]) -> Result<(), String> {
    let b = Command::new(slang()).arg("build").arg(src).arg("--target").arg("windows-amd64").arg("-o").arg(out).args(extra).output().unwrap();
    if !b.status.success() {
        return Err(format!("build failed:\n{}", String::from_utf8_lossy(&b.stderr)));
    }
    Ok(())
}

fn run_wine(exe: &Path) -> Output {
    wine().arg(exe).output().unwrap()
}

/// Source and expected output of example `name` (a file or a directory).
fn example(name: &str) -> (PathBuf, String) {
    let file = repo().join("examples").join(format!("{name}.sl"));
    if file.is_file() {
        return (file.clone(), std::fs::read_to_string(file.with_extension("out")).unwrap());
    }
    let dir = repo().join("examples").join(name);
    let out = std::fs::read_dir(&dir).unwrap().map(|e| e.unwrap().path()).find(|p| p.extension().is_some_and(|x| x == "out")).unwrap();
    (dir, std::fs::read_to_string(out).unwrap())
}

/// Builds every example for Windows and runs it under Wine; returns the
/// failures.
fn check_examples(names: &[&str], extra: &[&str]) -> Vec<String> {
    let dir = tmpdir(&format!("ex{}", extra.join("")));
    let mut failures = Vec::new();
    for name in names {
        let (src, expected) = example(name);
        let exe = dir.join(format!("{name}.exe"));
        if let Err(e) = build(&src, &exe, extra) {
            failures.push(format!("{name}: {e}"));
            continue;
        }
        let r = run_wine(&exe);
        let got = String::from_utf8_lossy(&r.stdout);
        if !r.status.success() {
            failures.push(format!("{name}: exit status {:?}, stderr:\n{}", r.status.code(), String::from_utf8_lossy(&r.stderr)));
        } else if got != expected {
            failures.push(format!("{name}: output differs\n--- expected\n{expected}--- got\n{got}"));
        }
    }
    let _ = std::fs::remove_dir_all(&dir);
    failures
}

/// Examples that run under Wine with the Linux `.out` byte for byte.
const WINDOWS_EXAMPLES: &[&str] = &[
    "hello", "arith", "control", "functions", "structs", "switch", "enums", "slices_maps", "strings", "interfaces",
    "errors", "errors_trace", "defer", "bank", "async", "cancel", "channels", "drop", "generics", "weak", "json",
    "reflection", "regex", "sealed_shapes", "try_block", "task_context", "tracing", "log", "metrics", "cli_tool",
    "stdlib_collections_algos", "stdlib_crypto", "stdlib_formats", "stdlib_io_encoding", "stdlib_money",
    "stdlib_strings2", "multi",
];

/// Networking examples (Winsock2 + the WSAPoll poller, loopback only).
/// http_realtime (HTTP/2, WebSocket, SSE) failed once with a connection
/// reset in the very first run on a fresh Wine prefix and passed in every
/// run since (16 of 17).
const WINDOWS_NET_EXAMPLES: &[&str] = &["tcp_echo", "http_server", "tls_echo", "redis", "http_realtime"];

/// Examples whose expected output is Linux-specific or that need what the
/// Windows port does not have (see the ignored test below).
const WINDOWS_NOT_APPLICABLE: &[(&str, &str)] = &[
    ("cfg", "its windows variant calls a winapi namespace that does not exist (the example only type-checks the host variant)"),
    ("stdlib_time_os_fs", "std.fs paths use '\\\\' and drive letters on Windows; the .out has Unix paths"),
    ("sql", "std.database.sqlite links the system libsqlite3, which mingw-w64 does not ship"),
    ("cpuprofile", "the CPU profiler (SIGPROF sampler) has no Windows backend yet"),
];

#[test]
fn windows_examples_under_wine() {
    if let Some(why) = windows_unavailable() {
        eprintln!("skipping: {why}");
        return;
    }
    let f = check_examples(WINDOWS_EXAMPLES, &["--leak-check"]);
    assert!(f.is_empty(), "{} example(s) failed for windows-amd64:\n{}", f.len(), f.join("\n"));
}

#[test]
fn windows_examples_release_under_wine() {
    if let Some(why) = windows_unavailable() {
        eprintln!("skipping: {why}");
        return;
    }
    let f = check_examples(&["hello", "structs", "errors", "async", "strings", "slices_maps", "json"], &["--release"]);
    assert!(f.is_empty(), "{} example(s) failed for windows-amd64 --release:\n{}", f.len(), f.join("\n"));
}

#[test]
fn windows_net_examples_under_wine() {
    if let Some(why) = windows_unavailable() {
        eprintln!("skipping: {why}");
        return;
    }
    let f = check_examples(WINDOWS_NET_EXAMPLES, &[]);
    assert!(f.is_empty(), "{} network example(s) failed for windows-amd64:\n{}", f.len(), f.join("\n"));
}

#[test]
#[ignore = "documents the examples that do not apply to Windows yet (each has a reason in WINDOWS_NOT_APPLICABLE); run with --ignored to see their state"]
fn windows_not_applicable_examples() {
    if let Some(why) = windows_unavailable() {
        eprintln!("skipping: {why}");
        return;
    }
    let names: Vec<&str> = WINDOWS_NOT_APPLICABLE.iter().map(|(n, _)| *n).collect();
    let f = check_examples(&names, &[]);
    for (n, why) in WINDOWS_NOT_APPLICABLE {
        let failed = f.iter().any(|m| m.starts_with(&format!("{n}:")));
        eprintln!("{n}: {} ({why})", if failed { "fails" } else { "passes" });
    }
}

/// os.Name/os.Arch/PathSeparator follow the target, the UTF-8 command line
/// arrives intact, non-ASCII output is UTF-8 bytes, and the exit code
/// passes through.
#[test]
fn windows_target_constants_args_and_exit_code() {
    if let Some(why) = windows_unavailable() {
        eprintln!("skipping: {why}");
        return;
    }
    let dir = tmpdir("consts");
    let src = dir.join("consts.sl");
    std::fs::write(
        &src,
        "namespace t.consts\n\nimport std\nimport std.io\n\nfunc main() {\n    println(os.Name, os.Arch, os.PathSeparator, len(os.LineSeparator))\n    var a = os.Args()\n    println(len(a), a[1], a[2])\n    println(\"h\u{e9}llo \u{2713}\")\n    os.Exit(3)\n}\n",
    )
    .unwrap();
    let exe = dir.join("consts.exe");
    build(&src, &exe, &[]).unwrap();
    let r = wine().arg(&exe).arg("x y").arg("\u{e9}").output().unwrap();
    assert_eq!(r.status.code(), Some(3), "stderr: {}", String::from_utf8_lossy(&r.stderr));
    assert_eq!(String::from_utf8_lossy(&r.stdout), "windows amd64 \\ 2\n3 x y \u{e9}\nh\u{e9}llo \u{2713}\n");
    // `slang run --target windows-amd64` goes through Wine.
    let r = Command::new(slang()).arg("run").arg("--target").arg("windows-amd64").arg(&src).arg("--").arg("p").arg("q").env("LC_ALL", "C.UTF-8").output().unwrap();
    assert_eq!(r.status.code(), Some(3));
    assert!(String::from_utf8_lossy(&r.stdout).starts_with("windows amd64"));
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn windows_exe_is_pe_and_default_name_has_exe_suffix() {
    if let Some(why) = windows_unavailable() {
        eprintln!("skipping: {why}");
        return;
    }
    let dir = tmpdir("pe");
    let r = Command::new(slang()).arg("build").arg("--target").arg("windows-amd64").arg("--release").arg(repo().join("examples/hello.sl")).current_dir(&dir).output().unwrap();
    assert!(r.status.success(), "{}", String::from_utf8_lossy(&r.stderr));
    let exe = dir.join("hello.exe");
    let bytes = std::fs::read(&exe).unwrap();
    assert_eq!(&bytes[..2], b"MZ");
    let pe = u32::from_le_bytes(bytes[0x3c..0x40].try_into().unwrap()) as usize;
    assert_eq!(&bytes[pe..pe + 4], b"PE\0\0");
    assert_eq!(u16::from_le_bytes(bytes[pe + 4..pe + 6].try_into().unwrap()), 0x8664, "machine: AMD64");
    let _ = std::fs::remove_dir_all(&dir);
}

/// Without a macOS SDK, `--emit=obj` still produces a Mach-O arm64 object,
/// and linking says what is missing.
#[test]
fn macos_arm64_emits_macho_object() {
    let dir = tmpdir("macos");
    let obj = dir.join("async.o");
    let r = Command::new(slang()).arg("build").arg("--target").arg("macos-arm64").arg("--emit=obj").arg(repo().join("examples/async.sl")).arg("-o").arg(&obj).output().unwrap();
    assert!(r.status.success(), "{}", String::from_utf8_lossy(&r.stderr));
    let b = std::fs::read(&obj).unwrap();
    assert_eq!(u32::from_le_bytes(b[0..4].try_into().unwrap()), 0xfeed_facf, "MH_MAGIC_64");
    assert_eq!(u32::from_le_bytes(b[4..8].try_into().unwrap()), 0x0100_000c, "CPU_TYPE_ARM64");
    assert_eq!(u32::from_le_bytes(b[12..16].try_into().unwrap()), 1, "MH_OBJECT");
    if std::env::var_os("SDKROOT").is_none() && !ok("xcrun", &["--version"]) {
        let r = Command::new(slang()).arg("build").arg("--target").arg("macos-arm64").arg(repo().join("examples/hello.sl")).arg("-o").arg(dir.join("hello")).output().unwrap();
        assert!(!r.status.success());
        assert!(String::from_utf8_lossy(&r.stderr).contains("macOS SDK"), "{}", String::from_utf8_lossy(&r.stderr));
    }
    let _ = std::fs::remove_dir_all(&dir);
}

/// Other targets' objects: ELF arm64 and COFF for the MSVC ABI.
#[test]
fn other_target_objects() {
    let dir = tmpdir("objs");
    for (target, magic) in [("linux-arm64", &b"\x7fELF"[..]), ("x86_64-pc-windows-msvc", &b"\x64\x86"[..])] {
        let obj = dir.join(format!("{target}.o"));
        let r = Command::new(slang()).arg("build").arg("--target").arg(target).arg("--emit=obj").arg(repo().join("examples/structs.sl")).arg("-o").arg(&obj).output().unwrap();
        assert!(r.status.success(), "{target}: {}", String::from_utf8_lossy(&r.stderr));
        let b = std::fs::read(&obj).unwrap();
        assert_eq!(&b[..magic.len()], magic, "{target}");
    }
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
#[ignore = "slow (builds and runs the 19 C test binaries under Wine, about two minutes); `cargo test --test cross -- --ignored` or `make -C runtime CROSS=x86_64-w64-mingw32 test-wine`; test_net is POSIX-only test code and is not built"]
fn runtime_c_tests_under_wine() {
    if let Some(why) = windows_unavailable() {
        eprintln!("skipping: {why}");
        return;
    }
    let r = Command::new("make").arg("-C").arg(repo().join("runtime")).arg("CROSS=x86_64-w64-mingw32").arg("test-wine").output().unwrap();
    let out = String::from_utf8_lossy(&r.stdout);
    let fail = out.lines().find(|l| l.starts_with("WINE FAIL:")).unwrap_or("");
    assert!(r.status.success() && fail.trim() == "WINE FAIL:", "C tests under wine:\n{out}");
}

/// On a console (a pseudo-terminal through `script`, which Wine turns into
/// a Windows console) the output goes through WriteConsoleW: the UTF-8
/// text must survive the UTF-16 conversion, including sequences split
/// across print calls. BMP characters only: Wine 9's console writes the
/// two halves of a surrogate pair (an emoji) as two U+FFFD, even for a
/// plain C WriteConsoleW call.
#[test]
fn windows_console_output_is_utf8() {
    if let Some(why) = windows_unavailable() {
        eprintln!("skipping: {why}");
        return;
    }
    if !ok("script", &["--version"]) {
        eprintln!("skipping: `script` (util-linux) is not installed");
        return;
    }
    let dir = tmpdir("console");
    let src = dir.join("console.sl");
    std::fs::write(&src, "namespace t.console\n\nimport std.io\n\nfunc main() {\n    print(\"h\u{e9}\")\n    println(\"llo w\u{f6}rld \u{2713} \u{4e16}\u{754c}\")\n}\n").unwrap();
    let exe = dir.join("console.exe");
    build(&src, &exe, &[]).unwrap();
    let r = Command::new("script").arg("-qc").arg(format!("wine {}", exe.display())).arg("/dev/null").env("WINEDEBUG", "-all").env("LC_ALL", "C.UTF-8").output().unwrap();
    let text = String::from_utf8_lossy(&r.stdout);
    // drop the console's ANSI control sequences
    let mut plain = String::new();
    let mut it = text.chars().peekable();
    while let Some(c) = it.next() {
        if c == '\u{1b}' {
            if it.peek() == Some(&'[') {
                it.next();
                for d in it.by_ref() {
                    if d.is_ascii_alphabetic() {
                        break;
                    }
                }
            }
            continue;
        }
        plain.push(c);
    }
    assert!(plain.contains("h\u{e9}llo w\u{f6}rld \u{2713} \u{4e16}\u{754c}"), "console output: {plain:?}");
    let _ = std::fs::remove_dir_all(&dir);
}
