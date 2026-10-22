//! The self-contained toolchain (docs/PLATFORMS.md, "Дистрибутив"): examples
//! built for every target by a *staged bundle* (bin/zell, bin/lld, targets/)
//! with PATH limited to its bin/, no clang, a fresh ZELL_HOME, and run here:
//! linux-amd64 (glibc, dynamic) and linux-amd64-musl (static) natively,
//! linux-arm64 / linux-arm64-musl under user-mode QEMU, windows-amd64 under
//! Wine; macos-arm64 / macos-amd64 linked and checked to be Mach-O
//! executables (CI runs them on the macOS runners).
//!
//! The bundle is `$ZELL_BUNDLE` (an unpacked toolchain directory, or a
//! `zell-*.tar.xz` / `.tar.gz` archive of scripts/package-toolchain.sh), else
//! one staged from this checkout: the test's `zell`, `build/lld/lld`
//! (scripts/build-lld.sh) and `build/targets/` (scripts/bundle-targets.sh).
//! Without either the tests print why and pass.

use super::{example, ok, repo, tmpdir};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::OnceLock;

/// Examples that need nothing from the machine they run on (no network,
/// SQLite, files, time zones) and print the same everywhere.
const EXAMPLES: &[&str] = &[
    "hello", "arith", "control", "functions", "structs", "switch", "enums", "slices_maps", "strings", "interfaces", "errors", "defer", "async", "channels",
    "generics", "json", "regex", "reflection", "try_block",
];

/// The staged toolchain directory, or why there is none.
fn bundle() -> Result<&'static Path, String> {
    static B: OnceLock<Result<PathBuf, String>> = OnceLock::new();
    B.get_or_init(stage).as_deref().map_err(Clone::clone)
}

fn stage() -> Result<PathBuf, String> {
    // one fixed directory under target/ (not /tmp), replaced on every run,
    // so staged toolchains (~200 MB) never pile up
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join("bundle-stage");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    if let Some(b) = std::env::var_os("ZELL_BUNDLE").filter(|v| !v.is_empty()) {
        let b = PathBuf::from(b);
        if b.is_dir() {
            return Ok(b);
        }
        let flag = if b.to_string_lossy().ends_with(".xz") { "-xJf" } else { "-xzf" };
        let st = Command::new("tar").arg(flag).arg(&b).arg("-C").arg(&dir).status().map_err(|e| format!("tar: {e}"))?;
        if !st.success() {
            return Err(format!("cannot unpack {}", b.display()));
        }
        let top = std::fs::read_dir(&dir).unwrap().flatten().map(|e| e.path()).find(|p| p.join("bin").is_dir()).ok_or("no bin/ in the archive")?;
        return Ok(top);
    }
    let lld = repo().join("build/lld/lld");
    let targets = repo().join("build/targets");
    if !lld.is_file() || !targets.join("x86_64-unknown-linux-gnu/BUNDLE").is_file() && !targets.join("aarch64-unknown-linux-gnu/BUNDLE").is_file() {
        return Err("no ZELL_BUNDLE and no staged inputs in build/ (run scripts/bundle-targets.sh and scripts/build-lld.sh)".into());
    }
    let tc = dir.join("zell");
    std::fs::create_dir_all(tc.join("bin")).unwrap();
    std::fs::copy(env!("CARGO_BIN_EXE_zell"), tc.join("bin/zell")).unwrap();
    std::fs::copy(&lld, tc.join("bin/lld")).unwrap();
    #[cfg(unix)]
    for f in ["ld.lld", "ld64.lld", "lld-link"] {
        std::os::unix::fs::symlink("lld", tc.join("bin").join(f)).unwrap();
    }
    let st = Command::new("cp").arg("-R").arg(&targets).arg(tc.join("targets")).status().unwrap();
    assert!(st.success());
    Ok(tc)
}

/// `zell` of the bundle with nothing else: PATH is only its bin/, a fresh
/// HOME/ZELL_HOME, no ZELL_* variables of the caller.
fn zell(tc: &Path) -> Command {
    let home = tc.parent().unwrap().join("home");
    std::fs::create_dir_all(&home).unwrap();
    let mut c = Command::new(tc.join("bin/zell"));
    c.env_clear().env("PATH", tc.join("bin")).env("HOME", &home).env("ZELL_HOME", home.join(".zell")).env("TMPDIR", std::env::temp_dir());
    c
}

fn has_target(tc: &Path, triple: &str) -> bool {
    tc.join("targets").join(triple).join("BUNDLE").is_file()
}

/// A runner for target `alias` on this machine, or why there is none.
fn runner(alias: &str) -> Result<Option<Command>, String> {
    let host_cpu = std::env::consts::ARCH;
    let linux = cfg!(target_os = "linux");
    match alias {
        "linux-amd64" | "linux-amd64-musl" if linux && host_cpu == "x86_64" => Ok(None),
        "linux-arm64" | "linux-arm64-musl" if linux && host_cpu == "aarch64" => Ok(None),
        "linux-arm64" | "linux-arm64-musl" if linux => {
            let q = ["qemu-aarch64-static", "qemu-aarch64"].into_iter().find(|q| ok(q, &["--version"])).ok_or("qemu-user-static is not installed")?;
            let mut c = Command::new(q);
            if alias == "linux-arm64" {
                // the dynamic loader and glibc of an arm64 system
                let root = std::env::var("ZELL_SYSROOT").unwrap_or_else(|_| "/usr/aarch64-linux-gnu".into());
                if !Path::new(&root).join("lib/ld-linux-aarch64.so.1").exists() {
                    return Err(format!("no arm64 glibc in {root} for QEMU (apt install libc6-arm64-cross, or set ZELL_SYSROOT)"));
                }
                c.arg("-L").arg(root);
            }
            Ok(Some(c))
        }
        "windows-amd64" if cfg!(windows) => Ok(None),
        "windows-amd64" if ok("wine", &["--version"]) => {
            let mut c = Command::new("wine");
            c.env("WINEDEBUG", "-all").env("LC_ALL", "C.UTF-8");
            Ok(Some(c))
        }
        "windows-amd64" => Err("wine is not installed".into()),
        "macos-arm64" if cfg!(target_os = "macos") && host_cpu == "aarch64" => Ok(None),
        "macos-amd64" if cfg!(target_os = "macos") => Ok(None), // Rosetta on arm64 Macs
        _ => Err(format!("{alias} executables cannot run on this machine")),
    }
}

/// Builds [`EXAMPLES`] for `alias` with the bundle (and `extra` flags) and
/// runs them; returns the failures. With `run` false (or no runner) only
/// links and checks the executable format.
fn check(alias: &str, triple: &str, extra: &[&str]) {
    let tc = match bundle() {
        Ok(tc) => tc,
        Err(why) => {
            eprintln!("skipping: {why}");
            return;
        }
    };
    if !has_target(tc, triple) {
        eprintln!("skipping: the bundle has no targets/{triple}");
        return;
    }
    let run = match runner(alias) {
        Ok(r) => Some(r),
        Err(why) => {
            eprintln!("{alias}: link only ({why})");
            None
        }
    };
    let dir = tmpdir(&format!("bundle-{alias}{}", extra.join("")));
    let mut failures = Vec::new();
    for name in EXAMPLES {
        let (src, expected) = example(name);
        let exe = dir.join(format!("{name}{}", if alias.starts_with("windows") { ".exe" } else { "" }));
        let out = zell(tc).arg("build").args(extra).arg("--target").arg(alias).arg(&src).arg("-o").arg(&exe).output().unwrap();
        if !out.status.success() {
            failures.push(format!("{name}: build failed:\n{}", String::from_utf8_lossy(&out.stderr)));
            continue;
        }
        let bytes = std::fs::read(&exe).unwrap();
        if let Err(e) = check_format(alias, &bytes) {
            failures.push(format!("{name}: {e}"));
            continue;
        }
        let Some(r) = &run else { continue };
        let mut cmd = match r {
            Some(c) => {
                let mut c2 = Command::new(c.get_program());
                c2.args(c.get_args());
                for (k, v) in c.get_envs() {
                    if let Some(v) = v {
                        c2.env(k, v);
                    }
                }
                c2.arg(&exe);
                c2
            }
            None => Command::new(&exe),
        };
        let r = cmd.env("ZELL_NO_BACKTRACE", "1").output().unwrap();
        let got = String::from_utf8_lossy(&r.stdout);
        if !r.status.success() {
            failures.push(format!("{name}: exit status {:?}, stderr:\n{}", r.status.code(), String::from_utf8_lossy(&r.stderr)));
        } else if got != expected {
            failures.push(format!("{name}: output differs\n--- expected\n{expected}--- got\n{got}"));
        }
    }
    let _ = std::fs::remove_dir_all(&dir);
    assert!(failures.is_empty(), "{} example(s) failed for {alias} with the bundle:\n{}", failures.len(), failures.join("\n"));
}

/// The executable's format, and for Linux what it needs from the system:
/// glibc ones no glibc symbol version above 2.31 and the standard dynamic
/// loader, musl ones no loader at all.
fn check_format(alias: &str, b: &[u8]) -> Result<(), String> {
    let has = |s: &[u8]| b.windows(s.len()).any(|w| w == s);
    match alias {
        a if a.starts_with("linux") => {
            if &b[..4] != b"\x7fELF" {
                return Err("not an ELF executable".into());
            }
            let interp = has(b"/lib64/ld-linux-x86-64.so.2") || has(b"/lib/ld-linux-aarch64.so.1");
            if a.ends_with("-musl") {
                if interp {
                    return Err("a musl executable must be fully static".into());
                }
            } else {
                if !interp {
                    return Err("no glibc dynamic loader".into());
                }
                for minor in 32..=60 {
                    if has(format!("GLIBC_2.{minor}\0").as_bytes()) {
                        return Err(format!("needs GLIBC_2.{minor}: the bundle's floor is glibc 2.31"));
                    }
                }
            }
        }
        a if a.starts_with("windows") => {
            if &b[..2] != b"MZ" {
                return Err("not a PE executable".into());
            }
        }
        a if a.starts_with("macos") => {
            if b[..4] != [0xcf, 0xfa, 0xed, 0xfe] || u32::from_le_bytes(b[12..16].try_into().unwrap()) != 2 {
                return Err("not a Mach-O executable (MH_EXECUTE)".into());
            }
        }
        _ => {}
    }
    Ok(())
}

#[test]
fn bundle_linux_amd64_glibc() {
    check("linux-amd64", "x86_64-unknown-linux-gnu", &[]);
    check("linux-amd64", "x86_64-unknown-linux-gnu", &["--release"]);
}

#[test]
fn bundle_linux_amd64_musl() {
    check("linux-amd64-musl", "x86_64-unknown-linux-musl", &["--release"]);
}

#[test]
fn bundle_linux_arm64_glibc() {
    check("linux-arm64", "aarch64-unknown-linux-gnu", &["--release"]);
}

#[test]
fn bundle_linux_arm64_musl() {
    check("linux-arm64-musl", "aarch64-unknown-linux-musl", &["--release"]);
}

#[test]
fn bundle_windows_amd64() {
    check("windows-amd64", "x86_64-pc-windows-gnu", &[]);
}

#[test]
fn bundle_macos() {
    check("macos-arm64", "aarch64-apple-darwin", &["--release"]);
    check("macos-amd64", "x86_64-apple-darwin", &["--release"]);
}

/// `zell doctor` of the bundle finds everything; `zell env` names the
/// bundled linker; the bundle links without clang even when asked for the
/// default linker.
#[test]
fn bundle_doctor_and_env() {
    let tc = match bundle() {
        Ok(tc) => tc,
        Err(why) => {
            eprintln!("skipping: {why}");
            return;
        }
    };
    let out = zell(tc).arg("doctor").output().unwrap();
    let text = String::from_utf8_lossy(&out.stdout).to_string() + &String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "zell doctor failed:\n{text}");
    assert!(text.contains("toolchain complete") && text.contains("multi-flavour"), "{text}");
    let out = zell(tc).args(["env", "ZELL_LINKER"]).output().unwrap();
    let linker = String::from_utf8_lossy(&out.stdout);
    assert!(linker.trim().ends_with("bin/lld") || linker.trim().ends_with("lld.exe"), "ZELL_LINKER={linker}");
    let list = zell(tc).args(["target", "list"]).output().unwrap();
    assert!(String::from_utf8_lossy(&list.stdout).contains("bundled"), "{}", String::from_utf8_lossy(&list.stdout));
}
