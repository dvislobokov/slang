//! The content-addressable store (docs/PACKAGES.md, "Хранилище и кэш"):
//!
//! ```text
//! $SLANG_STORE (default $SLANG_HOME/store, $SLANG_HOME = ~/.slang)
//!   files/<sha256>                          every file once, read-only
//!   packages/<module path>/<version>/index.yaml   path -> hash of one module version
//!   projects.txt                            project roots that installed from here (for prune)
//! ```
//!
//! A project's `.slang/deps/<module path>@<version>/` is filled with hard
//! links to `files/` (a copy when linking fails, e.g. across file
//! systems), so ten projects with one dependency store it once. Module
//! paths in directory names are case-encoded like Go (`!x` for `X`).

use super::sum::{self, FileEntry};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeSet, HashSet};
use std::path::{Path, PathBuf};

/// `$SLANG_HOME` or `~/.slang`.
pub fn slang_home() -> PathBuf {
    if let Some(h) = std::env::var_os("SLANG_HOME").filter(|v| !v.is_empty()) {
        return PathBuf::from(h);
    }
    let home = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")).map(PathBuf::from).unwrap_or_else(std::env::temp_dir);
    home.join(".slang")
}

/// `$SLANG_STORE` or `$SLANG_HOME/store`.
pub fn store_root() -> PathBuf {
    match std::env::var_os("SLANG_STORE").filter(|v| !v.is_empty()) {
        Some(s) => PathBuf::from(s),
        None => slang_home().join("store"),
    }
}

/// Go's case encoding: `A` -> `!a`, so paths stay distinct on
/// case-insensitive file systems.
pub fn escape_path(p: &str) -> String {
    let mut out = String::with_capacity(p.len());
    for c in p.chars() {
        if c.is_ascii_uppercase() {
            out.push('!');
            out.push(c.to_ascii_lowercase());
        } else {
            out.push(c);
        }
    }
    out
}

/// Inverse of [`escape_path`]; `None` for an invalid encoding.
pub fn unescape_path(p: &str) -> Option<String> {
    let mut out = String::with_capacity(p.len());
    let mut bang = false;
    for c in p.chars() {
        if bang {
            if !c.is_ascii_lowercase() {
                return None;
            }
            out.push(c.to_ascii_uppercase());
            bang = false;
        } else if c == '!' {
            bang = true;
        } else if c.is_ascii_uppercase() {
            return None;
        } else {
            out.push(c);
        }
    }
    (!bang).then_some(out)
}

/// Checks a module path: `/`-separated segments of `[A-Za-z0-9._~-]`,
/// not starting with `.` or `-`, no `..`.
pub fn valid_module_path(p: &str) -> bool {
    !p.is_empty()
        && p.split('/').all(|s| !s.is_empty() && !s.starts_with('.') && !s.starts_with('-') && s.chars().all(|c| c.is_ascii_alphanumeric() || "._~-+".contains(c)))
}

/// One file of an [`Index`].
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct IndexFile {
    pub path: String,
    pub sha256: String,
    pub size: u64,
}

/// `packages/<path>/<version>/index.yaml`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Index {
    pub module: String,
    pub version: String,
    /// The `h1:` hash of the module tree ([`sum::h1`]).
    pub h1: String,
    /// File name of the module's manifest (`slang.toml`, ...), if any.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub manifest: Option<String>,
    /// Where it was fetched from (`git+<url>#<commit>`, `proxy+<url>`).
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub origin: String,
    pub files: Vec<IndexFile>,
}

/// The global store.
#[derive(Clone, Debug)]
pub struct Store {
    pub root: PathBuf,
}

/// Name of the marker a materialised directory carries (its `h1:`).
pub const MARKER: &str = ".slang-h1";

fn write_atomic(path: &Path, data: &[u8]) -> Result<(), String> {
    let dir = path.parent().unwrap();
    std::fs::create_dir_all(dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    let tmp = dir.join(format!(".tmp-{}-{}", std::process::id(), path.file_name().unwrap().to_string_lossy()));
    std::fs::write(&tmp, data).map_err(|e| format!("cannot write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("cannot write {}: {e}", path.display()))
}

impl Store {
    /// The store at [`store_root`].
    pub fn open() -> Store {
        Store { root: store_root() }
    }

    pub fn at(root: impl Into<PathBuf>) -> Store {
        Store { root: root.into() }
    }

    pub fn file_path(&self, sha: &str) -> PathBuf {
        self.root.join("files").join(sha)
    }

    fn index_path(&self, module: &str, version: &str) -> PathBuf {
        self.root.join("packages").join(escape_path(module)).join(version).join("index.yaml")
    }

    /// The index of `module@version`, if stored.
    pub fn index(&self, module: &str, version: &str) -> Option<Index> {
        let text = std::fs::read_to_string(self.index_path(module, version)).ok()?;
        serde_yaml::from_str(&text).ok()
    }

    /// Adds a module version (files deduplicated by hash). Returns its index.
    pub fn add(&self, module: &str, version: &str, files: &[FileEntry], origin: &str) -> Result<Index, String> {
        let mut entries = Vec::new();
        for (p, data) in files {
            if !sum::valid_file_path(p) {
                return Err(format!("{module}@{version}: invalid file path `{p}`"));
            }
            let sha = sum::sha256_hex(data);
            let fp = self.file_path(&sha);
            let present = std::fs::metadata(&fp).map(|m| m.len() == data.len() as u64).unwrap_or(false);
            if !present {
                write_atomic(&fp, data)?;
                set_readonly(&fp);
            }
            entries.push(IndexFile { path: p.clone(), sha256: sha, size: data.len() as u64 });
        }
        entries.sort_by(|a, b| a.path.cmp(&b.path));
        let manifest = super::manifest::MANIFEST_NAMES.iter().find(|n| entries.iter().any(|e| e.path == **n)).map(|s| s.to_string());
        let idx = Index { module: module.into(), version: version.into(), h1: sum::h1(files), manifest, origin: origin.into(), files: entries };
        let text = serde_yaml::to_string(&idx).map_err(|e| e.to_string())?;
        write_atomic(&self.index_path(module, version), text.as_bytes())?;
        Ok(idx)
    }

    /// The bytes of one file of `idx` (hash-checked).
    pub fn read_file(&self, f: &IndexFile) -> Result<Vec<u8>, String> {
        let p = self.file_path(&f.sha256);
        let data = std::fs::read(&p).map_err(|e| format!("store file {} is missing: {e}; run `slang store verify`", p.display()))?;
        if sum::sha256_hex(&data) != f.sha256 {
            return Err(format!("store file {} is corrupted (hash mismatch); run `slang store verify`", p.display()));
        }
        Ok(data)
    }

    /// All files of a stored module, read back (hash-checked).
    pub fn files_of(&self, idx: &Index) -> Result<Vec<FileEntry>, String> {
        idx.files.iter().map(|f| Ok((f.path.clone(), self.read_file(f)?))).collect()
    }

    /// Fills `dest` with hard links to the files of `idx` (copies if
    /// linking fails). Skips the work when `dest` already holds this
    /// version (same marker). Returns whether anything was written.
    pub fn materialize(&self, idx: &Index, dest: &Path) -> Result<bool, String> {
        if std::fs::read_to_string(dest.join(MARKER)).ok().as_deref() == Some(idx.h1.as_str()) && idx.files.iter().all(|f| dest.join(&f.path).is_file()) {
            return Ok(false);
        }
        if dest.exists() {
            make_writable(dest);
            std::fs::remove_dir_all(dest).map_err(|e| format!("cannot clean {}: {e}", dest.display()))?;
        }
        for f in &idx.files {
            let src = self.file_path(&f.sha256);
            if !src.is_file() {
                return Err(format!("{}@{}: store file {} is missing; run `slang install` again", idx.module, idx.version, src.display()));
            }
            let to = dest.join(&f.path);
            std::fs::create_dir_all(to.parent().unwrap()).map_err(|e| format!("cannot create {}: {e}", to.parent().unwrap().display()))?;
            if std::fs::hard_link(&src, &to).is_err() {
                std::fs::copy(&src, &to).map_err(|e| format!("cannot copy {} to {}: {e}", src.display(), to.display()))?;
                set_readonly(&to);
            }
        }
        std::fs::write(dest.join(MARKER), &idx.h1).map_err(|e| format!("cannot write {}: {e}", dest.display()))?;
        Ok(true)
    }

    /// Records a project root that uses the store (for `prune`).
    pub fn register_project(&self, root: &Path) {
        let path = self.root.join("projects.txt");
        let mut set: BTreeSet<String> = std::fs::read_to_string(&path).unwrap_or_default().lines().map(str::to_string).collect();
        if set.insert(root.display().to_string()) {
            let text: String = set.into_iter().map(|l| l + "\n").collect();
            let _ = write_atomic(&path, text.as_bytes());
        }
    }

    /// Every stored (module, version).
    pub fn packages(&self) -> Vec<(String, String)> {
        let base = self.root.join("packages");
        let mut out = Vec::new();
        fn walk(base: &Path, dir: &Path, out: &mut Vec<(String, String)>) {
            let Ok(rd) = std::fs::read_dir(dir) else { return };
            for e in rd.flatten() {
                let p = e.path();
                if p.join("index.yaml").is_file() {
                    let rel = p.strip_prefix(base).unwrap();
                    let ver = rel.file_name().unwrap().to_string_lossy().to_string();
                    let module = rel.parent().unwrap().components().map(|c| c.as_os_str().to_string_lossy().to_string()).collect::<Vec<_>>().join("/");
                    out.push((unescape_path(&module).unwrap_or(module), ver));
                } else if p.is_dir() {
                    walk(base, &p, out);
                }
            }
        }
        walk(&base, &base, &mut out);
        out.sort();
        out
    }

    /// `slang store verify`: re-hashes every file and checks every index.
    /// Returns the problems found (empty when the store is sound).
    pub fn verify(&self) -> Vec<String> {
        let mut problems = Vec::new();
        let files = self.root.join("files");
        if let Ok(rd) = std::fs::read_dir(&files) {
            for e in rd.flatten() {
                let name = e.file_name().to_string_lossy().to_string();
                if name.starts_with(".tmp-") {
                    continue;
                }
                match std::fs::read(e.path()) {
                    Ok(d) if sum::sha256_hex(&d) == name => {}
                    Ok(_) => problems.push(format!("files/{name}: contents do not match the hash")),
                    Err(err) => problems.push(format!("files/{name}: {err}")),
                }
            }
        }
        for (m, v) in self.packages() {
            match self.index(&m, &v) {
                None => problems.push(format!("{m}@{v}: unreadable index.yaml")),
                Some(idx) => {
                    for f in &idx.files {
                        if !self.file_path(&f.sha256).is_file() {
                            problems.push(format!("{m}@{v}: {} is missing from files/", f.path));
                        }
                    }
                    let hashes: Vec<(String, String)> = idx.files.iter().map(|f| (f.path.clone(), f.sha256.clone())).collect();
                    if sum::h1_of_hashes(&hashes) != idx.h1 {
                        problems.push(format!("{m}@{v}: index does not match its h1 hash"));
                    }
                }
            }
        }
        problems
    }

    /// `slang store prune`: removes versions no registered project's
    /// `slang.lock` mentions, then files no index references. Returns
    /// (removed versions, removed files).
    pub fn prune(&self) -> (Vec<String>, usize) {
        let mut keep: HashSet<(String, String)> = HashSet::new();
        let mut live_projects = Vec::new();
        for root in std::fs::read_to_string(self.root.join("projects.txt")).unwrap_or_default().lines() {
            let Ok(text) = std::fs::read_to_string(Path::new(root).join("slang.lock")) else { continue };
            live_projects.push(root.to_string());
            if let Ok(lock) = super::lock::LockFile::parse(&text) {
                for p in lock.packages {
                    if let Some(m) = p.module {
                        keep.insert((m, p.version));
                    }
                }
            }
        }
        let text: String = live_projects.iter().map(|l| format!("{l}\n")).collect();
        let _ = write_atomic(&self.root.join("projects.txt"), text.as_bytes());
        let mut removed = Vec::new();
        for (m, v) in self.packages() {
            if !keep.contains(&(m.clone(), v.clone())) {
                let dir = self.index_path(&m, &v).parent().unwrap().to_path_buf();
                if std::fs::remove_dir_all(&dir).is_ok() {
                    removed.push(format!("{m}@{v}"));
                }
            }
        }
        let mut used = HashSet::new();
        for (m, v) in self.packages() {
            if let Some(idx) = self.index(&m, &v) {
                used.extend(idx.files.into_iter().map(|f| f.sha256));
            }
        }
        let mut n = 0;
        if let Ok(rd) = std::fs::read_dir(self.root.join("files")) {
            for e in rd.flatten() {
                let name = e.file_name().to_string_lossy().to_string();
                if !used.contains(&name) {
                    make_writable(&e.path());
                    if std::fs::remove_file(e.path()).is_ok() {
                        n += 1;
                    }
                }
            }
        }
        (removed, n)
    }
}

fn set_readonly(p: &Path) {
    if let Ok(m) = std::fs::metadata(p) {
        let mut perm = m.permissions();
        perm.set_readonly(true);
        let _ = std::fs::set_permissions(p, perm);
    }
}

/// Makes `p` (recursively) writable so it can be removed on systems where
/// read-only files resist deletion.
fn make_writable(p: &Path) {
    #[allow(clippy::permissions_set_readonly_false)]
    fn one(p: &Path) {
        if let Ok(m) = std::fs::symlink_metadata(p) {
            if m.is_dir() {
                if let Ok(rd) = std::fs::read_dir(p) {
                    for e in rd.flatten() {
                        one(&e.path());
                    }
                }
            } else if cfg!(windows) {
                let mut perm = m.permissions();
                perm.set_readonly(false);
                let _ = std::fs::set_permissions(p, perm);
            }
        }
    }
    one(p)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn case_encoding_round_trips() {
        assert_eq!(escape_path("github.com/Org/Pkg"), "github.com/!org/!pkg");
        assert_eq!(unescape_path("github.com/!org/!pkg").as_deref(), Some("github.com/Org/Pkg"));
        assert_eq!(unescape_path("a/B"), None);
        assert!(valid_module_path("example.com/org/pkg"));
        assert!(!valid_module_path("example.com/../x"));
        assert!(!valid_module_path("/abs"));
    }
}
