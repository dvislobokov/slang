//! Target runtimes of an installed toolchain (docs/PLATFORMS.md,
//! "Установка и цели"): `slang target list` and `slang target add`.
//!
//! Layout of a toolchain (`scripts/package-toolchain.sh`):
//!
//! ```text
//! <root>/bin/slang, bin/slang-lsp
//! <root>/std/...                      standard library sources (sema/stdfs.rs)
//! <root>/targets/<host triple>/libslang_rt.a
//! <root>/tools/...                    debugger formatters, editor files
//! ```
//!
//! Extra targets live in `$SLANG_TARGETS` (default `$SLANG_HOME/targets`,
//! i.e. `~/.slang/targets`), one directory per LLVM triple holding
//! `libslang_rt.a`. `slang target add <triple|alias>` downloads
//! `slang-runtime-<version>-<triple>.tar.gz` (which unpacks to
//! `<triple>/libslang_rt.a`) from the URL template `$SLANG_TARGETS_URL`
//! (default [`DEFAULT_URL`]; `{version}` and `{triple}` are substituted;
//! `https://` goes through curl, `http://` and `file://` are built in, a
//! plain path is a local archive), checks it against `<url>.sha256` when
//! the server has one, and unpacks it with the system `tar`.

use crate::target::Target;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Where `slang target add` downloads from unless `$SLANG_TARGETS_URL` is set.
pub const DEFAULT_URL: &str = "https://github.com/dvislobokov/slang/releases/download/v{version}/slang-runtime-{version}-{triple}.tar.gz";

/// Targets with prebuilt runtimes in a release (tier 1-2 of docs/PLATFORMS.md).
pub const KNOWN: &[(&str, &str)] = &[
    ("linux-amd64", "x86_64-unknown-linux-gnu"),
    ("linux-arm64", "aarch64-unknown-linux-gnu"),
    ("linux-amd64-musl", "x86_64-unknown-linux-musl"),
    ("linux-arm64-musl", "aarch64-unknown-linux-musl"),
    ("macos-arm64", "aarch64-apple-darwin"),
    ("macos-amd64", "x86_64-apple-darwin"),
    ("windows-amd64", "x86_64-pc-windows-gnu"),
];

/// The directory of downloaded target runtimes (`$SLANG_TARGETS`, else
/// `$SLANG_HOME/targets`).
pub fn targets_dir() -> PathBuf {
    if let Some(d) = std::env::var_os("SLANG_TARGETS").filter(|v| !v.is_empty()) {
        return PathBuf::from(d);
    }
    crate::package::store::slang_home().join("targets")
}

/// `<exe>/../targets` of an installed toolchain.
pub fn bundled_targets_dir() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    Some(exe.parent()?.parent()?.join("targets"))
}

/// Every place a runtime for `triple` is looked for, in order.
pub fn runtime_candidates(triple: &str) -> Vec<PathBuf> {
    let mut v = vec![targets_dir().join(triple).join("libslang_rt.a")];
    if let Some(b) = bundled_targets_dir() {
        v.push(b.join(triple).join("libslang_rt.a"));
    }
    v
}

fn url_for(template: &str, triple: &str) -> String {
    template.replace("{version}", env!("CARGO_PKG_VERSION")).replace("{triple}", triple)
}

fn fetch(src: &str) -> Result<Vec<u8>, String> {
    if !src.contains("://") {
        return std::fs::read(src).map_err(|e| format!("cannot read {src}: {e}"));
    }
    match crate::package::http::get(src)? {
        Some(b) => Ok(b),
        None => Err(format!("{src}: not found (no prebuilt runtime for this version and target; set SLANG_TARGETS_URL, or build one with `make -C runtime CROSS=...`)")),
    }
}

fn sha256_hex(b: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    crate::package::lock::hex(&Sha256::digest(b))
}

/// `slang target add <triple|alias>... [--url template]`.
pub fn add(args: &[String]) -> Result<String, String> {
    add_into(args, &targets_dir())
}

/// [`add`] into directory `dest`.
fn add_into(args: &[String], dest: &Path) -> Result<String, String> {
    let mut template = std::env::var("SLANG_TARGETS_URL").ok().filter(|s| !s.is_empty()).unwrap_or_else(|| DEFAULT_URL.to_string());
    let mut names = Vec::new();
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--url" => template = it.next().ok_or("--url needs a template")?.clone(),
            s if s.starts_with('-') => return Err(format!("unknown option {s} (usage: slang target add <target>... [--url template])")),
            s => names.push(s.to_string()),
        }
    }
    if names.is_empty() {
        return Err("usage: slang target add <target>... (see `slang target list`)".into());
    }
    std::fs::create_dir_all(dest).map_err(|e| format!("cannot create {}: {e}", dest.display()))?;
    let mut out = String::new();
    for n in names {
        let t = Target::parse(&n)?;
        let src = url_for(&template, &t.triple);
        let bytes = fetch(&src)?;
        if src.contains("://") {
            if let Ok(Some(sum)) = crate::package::http::get(&format!("{src}.sha256")) {
                let want = String::from_utf8_lossy(&sum).split_whitespace().next().unwrap_or("").to_ascii_lowercase();
                let got = sha256_hex(&bytes);
                if !want.is_empty() && want != got {
                    return Err(format!("{src}: checksum mismatch (sha256 {got}, expected {want})"));
                }
            }
        }
        let tmp = dest.join(format!(".download-{}-{}.tar.gz", t.triple, std::process::id()));
        std::fs::write(&tmp, &bytes).map_err(|e| format!("cannot write {}: {e}", tmp.display()))?;
        let st = Command::new("tar").arg("-xzf").arg(&tmp).arg("-C").arg(dest).output();
        let _ = std::fs::remove_file(&tmp);
        let st = st.map_err(|e| format!("cannot run tar: {e}"))?;
        if !st.status.success() {
            return Err(format!("unpacking {src} failed: {}", String::from_utf8_lossy(&st.stderr).trim()));
        }
        let lib = dest.join(&t.triple).join("libslang_rt.a");
        if !lib.is_file() {
            return Err(format!("{src} does not contain {}/libslang_rt.a", t.triple));
        }
        out.push_str(&format!("installed {} ({}) in {}\n", t.triple, n, lib.parent().unwrap().display()));
    }
    Ok(out)
}

/// Where the runtime of `t` is (any of the places the compiler looks), if anywhere.
fn installed(t: &Target) -> Option<PathBuf> {
    if t.is_host() {
        return crate::build::find_runtime().ok();
    }
    crate::crosslink::find_runtime(t).ok()
}

/// `slang target list`: the known targets and where their runtime is.
pub fn list(_args: &[String]) -> Result<String, String> {
    let mut out = format!("{:<20} {:<28} {}\n", "target", "triple", "runtime");
    let host = Target::host();
    let mut seen = Vec::new();
    let mut rows: Vec<(String, String)> = KNOWN.iter().map(|(a, t)| (a.to_string(), t.to_string())).collect();
    // runtimes downloaded or built for other triples too
    for dir in [Some(targets_dir()), bundled_targets_dir()].into_iter().flatten() {
        if let Ok(rd) = std::fs::read_dir(&dir) {
            for e in rd.flatten() {
                let name = e.file_name().to_string_lossy().to_string();
                if e.path().join("libslang_rt.a").is_file() && !rows.iter().any(|(_, t)| *t == name) {
                    rows.push((String::new(), name));
                }
            }
        }
    }
    for (alias, triple) in rows {
        if seen.contains(&triple) {
            continue;
        }
        seen.push(triple.clone());
        let t = Target::parse(&triple)?;
        let what = match installed(&t) {
            Some(p) => p.display().to_string(),
            None => "not installed (slang target add ".to_string() + if alias.is_empty() { &triple } else { &alias } + ")",
        };
        let mark = if t.triple == host.triple { " (host)" } else { "" };
        out.push_str(&format!("{:<20} {:<28} {what}{mark}\n", alias, triple));
    }
    out.push_str(&format!("downloads go to {} (SLANG_TARGETS); source: {}\n", targets_dir().display(), std::env::var("SLANG_TARGETS_URL").unwrap_or_else(|_| DEFAULT_URL.into())));
    Ok(out)
}

/// `slang target <list|add> ...`.
pub fn run(args: &[String]) -> Result<String, String> {
    match args.first().map(String::as_str) {
        Some("list") | None => list(&args[args.len().min(1)..]),
        Some("add") => add(&args[1..]),
        Some(c) => Err(format!("unknown subcommand `target {c}` (expected list or add)")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn url_template_and_known_targets() {
        let u = url_for(DEFAULT_URL, "x86_64-pc-windows-gnu");
        assert!(u.ends_with(&format!("slang-runtime-{}-x86_64-pc-windows-gnu.tar.gz", env!("CARGO_PKG_VERSION"))), "{u}");
        for (alias, triple) in KNOWN {
            assert_eq!(Target::parse(alias).unwrap().triple, *triple, "{alias}");
        }
    }

    #[test]
    fn add_unpacks_a_local_archive() {
        let dir = std::env::temp_dir().join(format!("slang-target-add-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("src/x86_64-unknown-linux-musl")).unwrap();
        std::fs::write(dir.join("src/x86_64-unknown-linux-musl/libslang_rt.a"), b"!<arch>\n").unwrap();
        let ar = dir.join("rt.tar.gz");
        let st = Command::new("tar").arg("-czf").arg(&ar).arg("-C").arg(dir.join("src")).arg("x86_64-unknown-linux-musl").status();
        if !st.is_ok_and(|s| s.success()) {
            return; // no tar here
        }
        let targets = dir.join("targets");
        let msg = add_into(&["linux-amd64-musl".into(), "--url".into(), ar.display().to_string()], &targets).unwrap();
        assert!(msg.contains("installed x86_64-unknown-linux-musl"), "{msg}");
        assert!(targets.join("x86_64-unknown-linux-musl/libslang_rt.a").is_file());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
