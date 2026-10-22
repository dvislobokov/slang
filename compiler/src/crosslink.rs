//! Cross-compilation support of `zell build --target` (docs/PLATFORMS.md):
//! where the runtime archive of a target lives, how an executable for it is
//! linked, and how one is run on this machine (Wine for Windows).
//!
//! - **Runtime**: `$ZELL_TARGETS/<triple>/libzell_rt.a` (default
//!   `~/.zell/targets/<triple>/`, filled by `zell target add`), the
//!   toolchain's `targets/<triple>/`, then `runtime/build/<triple>/libzell_rt.a` next to the
//!   compiler or in the repository it was built from (`make -C runtime
//!   CROSS=x86_64-w64-mingw32` puts the Windows archive there).
//! - **Windows (MinGW ABI)**: `clang -target <cpu>-w64-mingw32`, which finds
//!   the mingw-w64 CRT and import libraries (Debian/Ubuntu `mingw-w64`,
//!   MSYS2), linking with `lld` when installed (`-fuse-ld=lld`), else the
//!   mingw binutils `ld`. System libraries: ws2_32, iphlpapi, bcrypt,
//!   shell32, advapi32 (crypt32, dbghelp, ntdll and winsqlite3 are loaded
//!   at run time); `-lsqlite3` is dropped (winsqlite3.dll at run time).
//! - **macOS**: needs an SDK (`$SDKROOT`, or `xcrun --show-sdk-path` on a
//!   Mac) for `libSystem.tbd`; without one only `--emit=obj` works.
//! - **Other ELF targets**: `clang --target=<triple> -fuse-ld=lld` with
//!   `--sysroot=$ZELL_SYSROOT` when set (Debian/Ubuntu's
//!   `gcc-aarch64-linux-gnu` is found without one). Run here through
//!   `qemu-<arch>` (`$ZELL_QEMU`, `-L $ZELL_SYSROOT` or
//!   `/usr/<cpu>-linux-gnu` for the dynamic loader).
//! - **musl** (`linux-amd64-musl`): always `-static`; linked by `musl-gcc`
//!   (`$ZELL_MUSL_CC`; Debian/Ubuntu `musl-tools`) when it targets the
//!   same CPU, else clang with `--sysroot=$ZELL_SYSROOT` (a musl sysroot).
//!   The runtime comes from `make -C runtime CROSS=x86_64-linux-musl`.

use crate::target::{ObjectFormat, Target};
use std::path::{Path, PathBuf};
use std::process::Command;

/// Windows system libraries the runtime imports from.
pub const WINDOWS_LIBS: &[&str] = &["ws2_32", "iphlpapi", "bcrypt", "shell32", "advapi32"];

/// The directory of prebuilt target runtimes (`$ZELL_TARGETS`, default
/// `$ZELL_HOME/targets` = `~/.zell/targets`; `zell target add` fills it).
pub fn targets_dir() -> Option<PathBuf> {
    Some(crate::toolchain::targets_dir())
}

/// Locates `libzell_rt.a` built for `t` (not the host): `$ZELL_TARGETS`,
/// the toolchain's own `targets/`, then the repository's `runtime/build/`.
pub fn find_runtime(t: &Target) -> Result<PathBuf, String> {
    let mut cands = crate::toolchain::runtime_candidates(&t.triple);
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            cands.push(dir.join("../../runtime/build").join(&t.triple).join("libzell_rt.a"));
            cands.push(dir.join("../../../runtime/build").join(&t.triple).join("libzell_rt.a"));
        }
    }
    cands.push(Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime/build").join(&t.triple).join("libzell_rt.a"));
    for c in &cands {
        if c.is_file() {
            return Ok(c.canonicalize().unwrap_or(c.clone()));
        }
    }
    let hint = if t.os == "windows" { "make -C runtime CROSS=x86_64-w64-mingw32".to_string() } else { format!("make -C runtime CROSS={} (needs the target's C headers)", t.triple) };
    Err(format!(
        "no Zell runtime for {} (download it with `zell target add {}`, or build it with `{hint}`); looked in: {}",
        t.triple,
        t.triple,
        cands.iter().map(|c| c.display().to_string()).collect::<Vec<_>>().join(", ")
    ))
}

/// The macOS SDK: `$SDKROOT`, else `xcrun --show-sdk-path` (on a Mac).
fn macos_sdk() -> Option<String> {
    std::env::var("SDKROOT").ok().filter(|s| !s.is_empty()).or_else(|| {
        let out = Command::new("xcrun").args(["--sdk", "macosx", "--show-sdk-path"]).output().ok()?;
        out.status.success().then(|| String::from_utf8_lossy(&out.stdout).trim().to_string())
    })
}

/// Checks that an executable for `t` can be linked here at all (before
/// looking for its runtime), with the reason and the way out if not.
pub fn preflight(t: &Target) -> Result<(), String> {
    match t.object_format() {
        ObjectFormat::MachO if macos_sdk().is_none() => Err(format!(
            "linking for {} needs the macOS SDK (set SDKROOT to a MacOSX.sdk directory, or link on a Mac) and a runtime built with it; `--emit=obj` builds the object file without either",
            t.triple
        )),
        ObjectFormat::Coff if t.is_msvc() => Err(format!(
            "linking for {} (MSVC ABI) needs the Windows SDK and an MSVC-built runtime, not supported yet; use windows-amd64 (MinGW ABI) or --emit=obj",
            t.triple
        )),
        ObjectFormat::Wasm => Err(format!("linking for {} is not supported yet; use --emit=obj", t.triple)),
        _ => Ok(()),
    }
}

fn have(tool: &str) -> bool {
    Command::new(tool).arg("--version").output().is_ok_and(|o| o.status.success())
}

/// The musl-gcc wrapper for a musl target of the host's CPU
/// (`$ZELL_MUSL_CC`, else `musl-gcc` when installed).
fn musl_cc(t: &Target) -> Option<String> {
    if let Ok(cc) = std::env::var("ZELL_MUSL_CC") {
        return Some(cc);
    }
    let host_cpu = std::env::consts::ARCH;
    let cpu = t.triple.split('-').next().unwrap_or("");
    (cpu == host_cpu && have("musl-gcc")).then(|| "musl-gcc".to_string())
}

/// The clang `-target` of `t` for linking: MinGW triples are spelled the
/// way the mingw-w64 sysroot is named.
fn clang_target(t: &Target) -> String {
    if t.os == "windows" && !t.is_msvc() {
        let cpu = t.triple.split('-').next().unwrap_or("x86_64");
        return format!("{cpu}-w64-mingw32");
    }
    t.triple.clone()
}

/// Options of a link that depend on the build.
pub struct LinkOpts<'a> {
    pub release: bool,
    /// Strip symbols and debug information (`-s`).
    pub strip: bool,
    pub static_link: bool,
    pub clang: &'a str,
}

/// The link command for executable `output` of target `t` from `objs`, the
/// target runtime `rt` and the system libraries `libs`.
pub fn link_command(t: &Target, objs: &[PathBuf], rt: &Path, output: &Path, libs: &[&str], o: &LinkOpts) -> Result<Command, String> {
    if t.is_musl() {
        if let Some(cc) = musl_cc(t) {
            let mut cmd = Command::new(cc);
            cmd.args(objs).arg(rt);
            for l in libs.iter().filter(|l| **l != "sqlite3") {
                cmd.arg(format!("-l{l}"));
            }
            cmd.arg("-static").arg("-lpthread").arg("-lm");
            if o.release {
                cmd.arg("-Wl,--gc-sections");
            }
            if o.strip {
                cmd.arg("-s");
            }
            cmd.arg("-o").arg(output);
            return Ok(cmd);
        }
    }
    let mut cmd = Command::new(o.clang);
    cmd.arg("-target").arg(clang_target(t));
    cmd.args(objs).arg(rt);
    for l in libs {
        // Windows: SQLite is bound at run time from winsqlite3.dll / sqlite3.dll
        // (runtime/src/platform/sqlite_dyn.h), there is no import library to link;
        // musl: no static libsqlite3, the runtime is built without it.
        if (t.os == "windows" || t.is_musl()) && *l == "sqlite3" {
            continue;
        }
        cmd.arg(format!("-l{l}"));
    }
    match t.object_format() {
        ObjectFormat::Coff => {
            preflight(t)?;
            if have("ld.lld") || have("ld.lld-18") {
                cmd.arg("-fuse-ld=lld");
            }
            for l in WINDOWS_LIBS {
                cmd.arg(format!("-l{l}"));
            }
            if o.release {
                cmd.arg("-Wl,--gc-sections");
            }
            if o.static_link {
                cmd.arg("-static");
            }
        }
        ObjectFormat::MachO => {
            let Some(sdk) = macos_sdk() else { return Err(preflight(t).unwrap_err()) };
            cmd.arg("-isysroot").arg(sdk);
            if have("ld64.lld") || have("ld64.lld-18") {
                cmd.arg("-fuse-ld=lld");
            }
            if o.release {
                cmd.arg("-Wl,-dead_strip");
            }
        }
        ObjectFormat::Elf => {
            cmd.arg("-fuse-ld=lld");
            if let Ok(s) = std::env::var("ZELL_SYSROOT") {
                cmd.arg(format!("--sysroot={s}"));
            }
            cmd.arg("-lpthread");
            if o.release {
                cmd.arg("-Wl,--gc-sections");
            }
            if o.static_link || t.is_musl() {
                cmd.arg("-static");
            }
        }
        ObjectFormat::Wasm => return Err(preflight(t).unwrap_err()),
    }
    cmd.arg("-lm").arg("-o").arg(output);
    if o.strip {
        cmd.arg("-s");
    }
    Ok(cmd)
}

/// A command that runs executable `exe` of target `t` here: directly for
/// the host, through Wine (`$ZELL_WINE`, else `wine`) for Windows on
/// another OS; `None` if this machine cannot run it.
pub fn run_command(t: &Target, exe: &Path) -> Option<Command> {
    if t.is_host() {
        return Some(Command::new(exe));
    }
    let host = Target::host();
    if t.os == "linux" && host.os == "linux" {
        if t.arch == host.arch {
            return Some(Command::new(exe)); // musl: a static executable
        }
        // another CPU: user-mode QEMU, with the target's dynamic loader
        let cpu = t.triple.split('-').next().unwrap_or("");
        let cands = match std::env::var("ZELL_QEMU") {
            Ok(q) => vec![q],
            Err(_) => vec![format!("qemu-{cpu}-static"), format!("qemu-{cpu}")],
        };
        let qemu = cands.into_iter().find(|q| Command::new(q).arg("--version").output().is_ok_and(|o| o.status.success()))?;
        let mut c = Command::new(qemu);
        let root = std::env::var("ZELL_SYSROOT").unwrap_or_else(|_| format!("/usr/{cpu}-linux-gnu"));
        if !t.is_musl() && Path::new(&root).is_dir() {
            c.arg("-L").arg(root);
        }
        c.arg(exe);
        return Some(c);
    }
    if t.os == "windows" && t.arch == "amd64" && !cfg!(windows) {
        let wine = std::env::var("ZELL_WINE").unwrap_or_else(|_| "wine".into());
        if Command::new(&wine).arg("--version").output().is_ok_and(|o| o.status.success()) {
            let mut c = Command::new(wine);
            c.arg(exe);
            if std::env::var_os("WINEDEBUG").is_none() {
                c.env("WINEDEBUG", "-all");
            }
            return Some(c);
        }
    }
    None
}
