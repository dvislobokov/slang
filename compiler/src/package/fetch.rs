//! Getting module versions: the version list, the manifest and the file
//! tree of `path@version`, through the proxies of `ZELL_PROXY` or
//! directly from git, verified against `zell.lock` (by the caller) and the
//! checksum log (`ZELL_SUMDB`), and kept in the [`Store`].
//!
//! # Environment
//!
//! | Variable | Meaning | Default |
//! |---|---|---|
//! | `ZELL_PROXY` | proxies to try, `,`-separated (next on 404/410) or `\|`-separated (next on any error); `direct` = git, `off` = fail | `direct` |
//! | `ZELL_PRIVATE` | globs of module paths fetched directly with system credentials, never through a proxy or the checksum log (`github.com/mycorp/*`) | — |
//! | `ZELL_NOPROXY` | globs fetched directly (but still checked against the log) | `ZELL_PRIVATE` |
//! | `ZELL_SUMDB` | base URL of the checksum log, or `off` | `off` |
//! | `ZELL_NOSUMDB` | globs not checked against the log | `ZELL_PRIVATE` |
//!
//! A glob matches a path prefix with the same number of `/`-segments, each
//! segment by shell rules (Go's `GOPRIVATE` semantics).
//!
//! # Proxy protocol (static layout, any HTTP server can serve it)
//!
//! ```text
//! GET <proxy>/<path>/@v/list               versions, one per line (`v1.2.0`)
//! GET <proxy>/<path>/@v/<version>.info     {"Version":"v1.2.0","Time":"...","Manifest":"zell.toml","Origin":"..."}
//! GET <proxy>/<path>/@v/<version>.manifest the module's manifest file, verbatim
//! GET <proxy>/<path>/@v/<version>.zip      the tree under `<path>@<version>/`, stored (see [`super::sum`])
//! ```
//!
//! `<path>` is case-encoded ([`super::store::escape_path`]).
//!
//! # Checksum log (`ZELL_SUMDB`), format v1
//!
//! ```text
//! GET <sumdb>/latest                 "zell-sumdb v1\n<N>\n<base64 sha256 of the first N records>\n"
//! GET <sumdb>/lookup/<path>@<version>
//!                                    "<record number>\n<path> <version> h1:...\n<path> <version>/manifest h1:...\n"
//! GET <sumdb>/log                    every record, in order (two lines each, as above)
//! ```
//!
//! The log is append-only; a record never changes. The client checks that
//! the lookup's record number is below the tree size of `/latest` and that
//! the hash matches what it downloaded; a mismatch is a security error.
//! Version 1 trusts the server for the history (no Merkle inclusion
//! proofs, no signed tree heads): `/latest` is a note whose first line
//! names the format, so v2 can add a Merkle root and signature lines and
//! `/tile/...` endpoints without changing lookups.

use super::git::{self, GitRef};
use super::semver::Version;
use super::store::{escape_path, Index, Store};
use super::sum::{self, FileEntry};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Mutex;

#[derive(Clone, Debug, PartialEq, Eq)]
enum Entry {
    Direct,
    Off,
    Url(String),
}

/// Module-fetching configuration (from the environment).
#[derive(Clone, Debug)]
pub struct Config {
    /// (entry, fall through on any error).
    proxies: Vec<(Entry, bool)>,
    private: Vec<String>,
    noproxy: Vec<String>,
    nosumdb: Vec<String>,
    pub sumdb: Option<String>,
}

fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.trim().is_empty())
}

fn globs(s: Option<String>) -> Vec<String> {
    s.map(|s| s.split(',').map(|g| g.trim().trim_end_matches('/').to_string()).filter(|g| !g.is_empty()).collect()).unwrap_or_default()
}

/// Whether module `path` matches one of `globs` (prefix by segments).
pub fn matches_globs(globs: &[String], path: &str) -> bool {
    let segs: Vec<&str> = path.split('/').collect();
    globs.iter().any(|g| {
        let gs: Vec<&str> = g.split('/').collect();
        gs.len() <= segs.len() && gs.iter().zip(&segs).all(|(p, s)| glob::Pattern::new(p).map(|p| p.matches(s)).unwrap_or(false))
    })
}

impl Config {
    pub fn from_env() -> Config {
        let spec = env("ZELL_PROXY").unwrap_or_else(|| "direct".into());
        let mut proxies = Vec::new();
        let mut cur = String::new();
        for c in spec.chars().chain(std::iter::once(',')) {
            if c == ',' || c == '|' {
                let e = cur.trim().trim_end_matches('/').to_string();
                cur.clear();
                if e.is_empty() {
                    continue;
                }
                let entry = match e.as_str() {
                    "direct" => Entry::Direct,
                    "off" => Entry::Off,
                    _ => Entry::Url(e),
                };
                proxies.push((entry, c == '|'));
            } else {
                cur.push(c);
            }
        }
        let private = globs(env("ZELL_PRIVATE"));
        let noproxy = env("ZELL_NOPROXY").map(|s| globs(Some(s))).unwrap_or_else(|| private.clone());
        let nosumdb = env("ZELL_NOSUMDB").map(|s| globs(Some(s))).unwrap_or_else(|| private.clone());
        let sumdb = env("ZELL_SUMDB").filter(|s| s != "off").map(|s| s.trim_end_matches('/').to_string());
        Config { proxies, private, noproxy, nosumdb, sumdb }
    }

    /// The proxies for `path` (only `direct` for private / noproxy paths).
    fn route(&self, path: &str) -> Vec<(Entry, bool)> {
        if matches_globs(&self.private, path) || matches_globs(&self.noproxy, path) {
            return vec![(Entry::Direct, false)];
        }
        self.proxies.clone()
    }

    fn checks_sum(&self, path: &str) -> bool {
        self.sumdb.is_some() && !matches_globs(&self.private, path) && !matches_globs(&self.nosumdb, path)
    }

    /// `ZELL_PROXY` as given (for `zell env`).
    pub fn proxy_spec() -> String {
        env("ZELL_PROXY").unwrap_or_else(|| "direct".into())
    }
}

/// A fetched or stored module version.
#[derive(Clone, Debug)]
pub struct Fetched {
    pub index: Index,
    /// For git pins: the commit.
    pub commit: Option<String>,
}

/// Fetches modules (with per-process caches of version lists).
pub struct Fetcher {
    pub config: Config,
    pub store: Store,
    pub offline: bool,
    lists: Mutex<HashMap<String, Vec<Version>>>,
    tags: Mutex<HashMap<String, Vec<git::Tag>>>,
    /// Checksums from `zell.lock` by (module, version): a download that
    /// does not match is rejected before it reaches the store.
    expected: Mutex<HashMap<(String, String), String>>,
}

fn tmp_dir(what: &str) -> PathBuf {
    use std::sync::atomic::{AtomicU64, Ordering};
    static N: AtomicU64 = AtomicU64::new(0);
    std::env::temp_dir().join(format!("zell-{what}-{}-{}", std::process::id(), N.fetch_add(1, Ordering::Relaxed)))
}

impl Fetcher {
    pub fn new(offline: bool) -> Fetcher {
        Fetcher { config: Config::from_env(), store: Store::open(), offline, lists: Mutex::default(), tags: Mutex::default(), expected: Mutex::default() }
    }

    /// Records the locked checksum of `path@version`.
    pub fn expect(&self, path: &str, version: &str, h1: &str) {
        self.expected.lock().unwrap().insert((path.to_string(), version.to_string()), h1.to_string());
    }

    /// Checks a downloaded tree against the lock, then adds it to the store.
    fn add(&self, path: &str, version: &str, files: &[FileEntry], origin: &str) -> Result<Index, String> {
        let h = sum::h1(files);
        if let Some(want) = self.expected.lock().unwrap().get(&(path.to_string(), version.to_string())) {
            if *want != h {
                return Err(format!(
                    "SECURITY ERROR: checksum mismatch for {path}@{version}\n  downloaded: {h}\n  zell.lock: {want}\nthe module content differs from the locked one; it may have been tampered with"
                ));
            }
        }
        self.store.add(path, version, files, origin)
    }

    fn net(&self, what: &str) -> Result<(), String> {
        if self.offline {
            return Err(format!("{what}: needs the network, but --offline was given (run `zell install` online first)"));
        }
        Ok(())
    }

    fn git_tags(&self, url: &str) -> Result<Vec<git::Tag>, String> {
        if let Some(t) = self.tags.lock().unwrap().get(url) {
            return Ok(t.clone());
        }
        let t = git::list_tags(url)?;
        self.tags.lock().unwrap().insert(url.to_string(), t.clone());
        Ok(t)
    }

    /// Every release version of `path` (`url` overrides the git location).
    pub fn versions(&self, path: &str, url: Option<&str>) -> Result<Vec<Version>, String> {
        let key = format!("{path} {}", url.unwrap_or(""));
        if let Some(v) = self.lists.lock().unwrap().get(&key) {
            return Ok(v.clone());
        }
        self.net(&format!("listing versions of {path}"))?;
        let mut last_err = format!("{path}: no proxy in ZELL_PROXY could list it");
        let route = if url.is_some() { vec![(Entry::Direct, false)] } else { self.config.route(path) };
        let mut found = None;
        for (entry, any) in route {
            match entry {
                Entry::Off => return Err(format!("{path}: module lookup disabled by ZELL_PROXY=off")),
                Entry::Direct => {
                    let u = url.map(str::to_string).unwrap_or_else(|| git::url_of(path));
                    match self.git_tags(&u) {
                        Ok(t) => {
                            found = Some(t.into_iter().map(|t| t.version).collect::<Vec<_>>());
                            break;
                        }
                        Err(e) => last_err = format!("{path}: {e}"),
                    }
                }
                Entry::Url(base) => match super::http::get(&format!("{base}/{}/@v/list", escape_path(path))) {
                    Ok(Some(b)) => {
                        let mut vs: Vec<Version> = String::from_utf8_lossy(&b).lines().filter_map(|l| Version::parse(l.trim())).collect();
                        vs.sort();
                        vs.dedup();
                        found = Some(vs);
                        break;
                    }
                    Ok(None) => last_err = format!("{path}: not found on proxy {base}"),
                    Err(e) => {
                        last_err = e;
                        if !any {
                            return Err(last_err);
                        }
                    }
                },
            }
        }
        let vs = found.ok_or(last_err)?;
        self.lists.lock().unwrap().insert(key, vs.clone());
        Ok(vs)
    }

    /// The manifest (file name, bytes) of `path@version`: from the store
    /// when present, else from a proxy (`.info` + `.manifest`, checked
    /// against the log) or by downloading the module directly.
    pub fn manifest(&self, path: &str, version: &str, url: Option<&str>) -> Result<Option<(String, Vec<u8>)>, String> {
        if let Some(idx) = self.store.index(path, version) {
            return self.manifest_from_store(&idx);
        }
        self.net(&format!("fetching {path}@{version}"))?;
        if url.is_none() {
            for (entry, any) in self.config.route(path) {
                match entry {
                    Entry::Off => return Err(format!("{path}: module lookup disabled by ZELL_PROXY=off")),
                    Entry::Direct => break,
                    Entry::Url(base) => {
                        let pre = format!("{base}/{}/@v/{version}", escape_path(path));
                        let info = match super::http::get(&format!("{pre}.info")) {
                            Ok(Some(i)) => i,
                            Ok(None) => continue,
                            Err(e) if any => {
                                let _ = e;
                                continue;
                            }
                            Err(e) => return Err(e),
                        };
                        let info: serde_json::Value = serde_json::from_slice(&info).map_err(|e| format!("{pre}.info: {e}"))?;
                        let Some(name) = info.get("Manifest").and_then(|m| m.as_str()).map(str::to_string) else { return Ok(None) };
                        let bytes = super::http::get(&format!("{pre}.manifest"))?.ok_or_else(|| format!("{pre}.manifest: not found"))?;
                        self.check_log(path, version, "/manifest", &sum::h1(&[(name.clone(), bytes.clone())]))?;
                        return Ok(Some((name, bytes)));
                    }
                }
            }
        }
        let idx = self.download(path, version, url)?.index;
        self.manifest_from_store(&idx)
    }

    fn manifest_from_store(&self, idx: &Index) -> Result<Option<(String, Vec<u8>)>, String> {
        let Some(name) = &idx.manifest else { return Ok(None) };
        let f = idx.files.iter().find(|f| &f.path == name).ok_or("store index lists a manifest it does not contain")?;
        Ok(Some((name.clone(), self.store.read_file(f)?)))
    }

    /// The tree of `path@version` in the store (downloading it if needed).
    pub fn download(&self, path: &str, version: &str, url: Option<&str>) -> Result<Fetched, String> {
        if let Some(idx) = self.store.index(path, version) {
            return Ok(Fetched { index: idx, commit: None });
        }
        self.net(&format!("downloading {path}@{version}"))?;
        let mut last_err = format!("{path}@{version}: no proxy in ZELL_PROXY has it");
        let route = if url.is_some() { vec![(Entry::Direct, false)] } else { self.config.route(path) };
        for (entry, any) in route {
            match entry {
                Entry::Off => return Err(format!("{path}: module download disabled by ZELL_PROXY=off")),
                Entry::Direct => {
                    let u = url.map(str::to_string).unwrap_or_else(|| git::url_of(path));
                    let want = Version::parse(version).ok_or_else(|| format!("{path}: `{version}` is not a version"))?;
                    let tags = self.git_tags(&u)?;
                    let tag = tags.iter().find(|t| t.version == want).ok_or_else(|| format!("{path}: {u} has no tag for version {version}"))?;
                    let (files, commit) = checkout_tree(&u, &GitRef::Tag(tag.name.clone()))?;
                    let h = sum::h1(&files);
                    self.check_log(path, version, "", &h)?;
                    if let Some(m) = manifest_entry(&files) {
                        self.check_log(path, version, "/manifest", &sum::h1(&[m]))?;
                    }
                    let idx = self.add(path, version, &files, &format!("git+{u}#{}", commit.0))?;
                    return Ok(Fetched { index: idx, commit: Some(commit.0) });
                }
                Entry::Url(base) => {
                    let url = format!("{base}/{}/@v/{version}.zip", escape_path(path));
                    match super::http::get(&url) {
                        Ok(Some(z)) => {
                            let files = sum::zip_read(&z, &format!("{path}@{version}")).map_err(|e| format!("{url}: {e}"))?;
                            self.check_log(path, version, "", &sum::h1(&files))?;
                            let idx = self.add(path, version, &files, &format!("proxy+{base}"))?;
                            return Ok(Fetched { index: idx, commit: None });
                        }
                        Ok(None) => last_err = format!("{path}@{version}: not found on proxy {base}"),
                        Err(e) => {
                            last_err = e;
                            if !any {
                                return Err(last_err);
                            }
                        }
                    }
                }
            }
        }
        Err(last_err)
    }

    /// A git pin (`{ git, tag|branch|rev }`): (version, fetched). Tags that
    /// are versions keep their version; anything else gets a
    /// pseudo-version `v0.0.0-<UTC commit time>-<12 hex of the commit>`.
    /// `locked` is (version, commit) from `zell.lock`, reused when the
    /// store still has it.
    pub fn git_pin(&self, path: &str, url: &str, r: &GitRef, locked: Option<(&str, &str)>) -> Result<(String, Fetched), String> {
        if let Some((v, c)) = locked {
            if let Some(idx) = self.store.index(path, v) {
                return Ok((v.to_string(), Fetched { index: idx, commit: Some(c.to_string()) }));
            }
            if !matches!(r, GitRef::Tag(_)) {
                // Re-fetch exactly the locked commit.
                self.net(&format!("fetching {path} ({})", r.describe()))?;
                let (files, (commit, _)) = checkout_tree(url, &GitRef::Rev(c.to_string()))?;
                let idx = self.add(path, v, &files, &format!("git+{url}#{commit}"))?;
                return Ok((v.to_string(), Fetched { index: idx, commit: Some(commit) }));
            }
        }
        if let GitRef::Tag(t) = r {
            if let Some(v) = Version::parse(t) {
                let version = v.tag();
                if let Some(idx) = self.store.index(path, &version) {
                    return Ok((version, Fetched { index: idx, commit: locked.map(|l| l.1.to_string()) }));
                }
            }
        }
        self.net(&format!("fetching {path} ({})", r.describe()))?;
        let (files, (commit, time)) = checkout_tree(url, r)?;
        let version = match r {
            GitRef::Tag(t) if Version::parse(t).is_some() => Version::parse(t).unwrap().tag(),
            _ => format!("v0.0.0-{time}-{}", &commit[..12.min(commit.len())]),
        };
        let idx = match self.store.index(path, &version) {
            Some(i) => i,
            None => self.add(path, &version, &files, &format!("git+{url}#{commit}"))?,
        };
        Ok((version, Fetched { index: idx, commit: Some(commit) }))
    }

    /// Checks `h1` of `path@version` (`kind` = "" or "/manifest") against
    /// the checksum log, when one is configured for `path`.
    pub fn check_log(&self, path: &str, version: &str, kind: &str, h1: &str) -> Result<(), String> {
        if !self.config.checks_sum(path) {
            return Ok(());
        }
        let base = self.config.sumdb.as_deref().unwrap();
        let url = format!("{base}/lookup/{}@{version}", escape_path(path));
        let body = super::http::get(&url)?.ok_or_else(|| format!("{path}@{version}: not found in the checksum log {base} (ZELL_NOSUMDB or ZELL_PRIVATE skip it for private modules)"))?;
        let text = String::from_utf8_lossy(&body).to_string();
        let mut lines = text.lines();
        let rec: u64 = lines.next().and_then(|l| l.trim().parse().ok()).ok_or_else(|| format!("{url}: malformed lookup response"))?;
        let latest = super::http::get(&format!("{base}/latest"))?.ok_or_else(|| format!("{base}/latest: not found"))?;
        let latest = String::from_utf8_lossy(&latest).to_string();
        let mut ll = latest.lines();
        if ll.next() != Some("zell-sumdb v1") {
            return Err(format!("{base}/latest: unknown checksum log format"));
        }
        let size: u64 = ll.next().and_then(|l| l.trim().parse().ok()).ok_or_else(|| format!("{base}/latest: malformed tree head"))?;
        if rec >= size {
            return Err(format!("{url}: record {rec} is beyond the log size {size}"));
        }
        let key = format!("{path} {version}{kind}");
        let logged = lines.find_map(|l| l.strip_prefix(&key).and_then(|r| r.strip_prefix(' '))).map(str::trim);
        match logged {
            Some(h) if h == h1 => Ok(()),
            Some(h) => Err(format!(
                "SECURITY ERROR: checksum mismatch for {path}@{version}{kind}\n  downloaded:    {h1}\n  checksum log:  {h}\nthe module content differs from what the log recorded; it may have been tampered with"
            )),
            None if kind == "/manifest" => Ok(()),
            None => Err(format!("{url}: no record for {key}")),
        }
    }
}

/// The manifest file of a tree, if any.
pub fn manifest_entry(files: &[FileEntry]) -> Option<FileEntry> {
    super::manifest::MANIFEST_NAMES.iter().find_map(|n| files.iter().find(|(p, _)| p == n).cloned())
}

/// Checks `r` out into a temporary directory and reads the tree.
/// Returns (files, (commit, commit time)).
pub fn checkout_tree(url: &str, r: &GitRef) -> Result<(Vec<FileEntry>, (String, String)), String> {
    let dir = tmp_dir("git");
    let res = git::checkout(url, r, &dir).and_then(|c| Ok((sum::read_tree(&dir)?, c)));
    let _ = std::fs::remove_dir_all(&dir);
    res
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn private_globs_match_by_segments() {
        let g = vec!["github.com/mycorp/*".to_string(), "*.internal".to_string()];
        assert!(matches_globs(&g, "github.com/mycorp/x"));
        assert!(matches_globs(&g, "github.com/mycorp/x/sub"));
        assert!(!matches_globs(&g, "github.com/other/x"));
        assert!(matches_globs(&g, "git.internal/a"));
        assert!(!matches_globs(&g, "github.com"));
    }
}
