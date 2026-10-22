//! Direct module access through the system `git` (the `direct` entry of
//! `ZELL_PROXY`, `ZELL_PRIVATE` paths and `{ git = ... }` dependencies).
//! Credentials, `url.<base>.insteadOf` rewrites and proxies are whatever
//! the user's git configuration says.

use super::semver::Version;
use std::path::Path;
use std::process::Command;

/// A git reference requested by a manifest.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum GitRef {
    Tag(String),
    Branch(String),
    Rev(String),
}

impl GitRef {
    pub fn describe(&self) -> String {
        match self {
            GitRef::Tag(t) => format!("tag {t}"),
            GitRef::Branch(b) => format!("branch {b}"),
            GitRef::Rev(r) => format!("rev {r}"),
        }
    }
}

/// The clone URL of a module path: `github.com/org/pkg` ->
/// `https://github.com/org/pkg`; URLs, scp-like `git@host:path` and local
/// paths are used as they are.
pub fn url_of(module_or_url: &str) -> String {
    let s = module_or_url;
    if s.contains("://") || s.starts_with('/') || s.starts_with('.') || (s.contains('@') && s.contains(':')) {
        s.to_string()
    } else {
        format!("https://{s}")
    }
}

/// The module path of a `git = "..."` value: the URL without scheme, user,
/// `.git` suffix and trailing `/` (`https://github.com/org/x.git` ->
/// `github.com/org/x`).
pub fn module_path_of(git: &str) -> String {
    let mut s = git.trim();
    if let Some((_, rest)) = s.split_once("://") {
        s = rest;
    }
    if let Some((user, rest)) = s.split_once('@') {
        if !user.contains('/') {
            s = rest;
        }
    }
    let mut out = s.replacen(':', "/", if git.contains("://") { 0 } else { 1 });
    while out.ends_with('/') {
        out.pop();
    }
    if let Some(x) = out.strip_suffix(".git") {
        out = x.to_string();
    }
    out.trim_start_matches('/').to_string()
}

fn git() -> Command {
    let mut c = Command::new("git");
    c.env("GIT_TERMINAL_PROMPT", "0").env("GIT_ASKPASS", "").env_remove("GIT_DIR").env_remove("GIT_WORK_TREE");
    c
}

fn run(mut c: Command, what: &str) -> Result<String, String> {
    let out = c.output().map_err(|e| format!("cannot run git ({what}): {e}; is git installed?"))?;
    if !out.status.success() {
        let err = String::from_utf8_lossy(&out.stderr);
        return Err(format!("git {what} failed: {}", err.trim()));
    }
    Ok(String::from_utf8_lossy(&out.stdout).to_string())
}

/// A semver tag of a repository.
#[derive(Clone, Debug)]
pub struct Tag {
    pub version: Version,
    /// The tag name as spelled in the repository (`v1.2.0` or `1.2.0`).
    pub name: String,
    pub commit: String,
}

/// `git ls-remote --tags`: every tag that is a semantic version.
pub fn list_tags(url: &str) -> Result<Vec<Tag>, String> {
    let mut c = git();
    c.args(["ls-remote", "--tags", url]);
    let out = run(c, &format!("ls-remote {url}"))?;
    let mut tags: Vec<Tag> = Vec::new();
    for line in out.lines() {
        let Some((sha, r)) = line.split_once('\t') else { continue };
        let Some(name) = r.strip_prefix("refs/tags/") else { continue };
        // Annotated tags: `name^{}` is the commit; it wins over the tag object.
        let (name, peeled) = match name.strip_suffix("^{}") {
            Some(n) => (n, true),
            None => (name, false),
        };
        let Some(version) = Version::parse(name) else { continue };
        if let Some(t) = tags.iter_mut().find(|t| t.name == name) {
            if peeled {
                t.commit = sha.to_string();
            }
            continue;
        }
        tags.push(Tag { version, name: name.to_string(), commit: sha.to_string() });
    }
    tags.sort_by(|a, b| a.version.cmp(&b.version).then(a.name.cmp(&b.name)));
    tags.dedup_by(|a, b| a.version == b.version);
    Ok(tags)
}

/// The commit a branch points to.
pub fn branch_commit(url: &str, branch: &str) -> Result<String, String> {
    let mut c = git();
    c.args(["ls-remote", "--heads", url, &format!("refs/heads/{branch}")]);
    let out = run(c, &format!("ls-remote {url}"))?;
    out.lines().next().and_then(|l| l.split('\t').next()).map(str::to_string).ok_or_else(|| format!("{url} has no branch `{branch}`"))
}

/// Checks `r` of `url` out into the new directory `dest` (shallow when
/// possible) and returns (commit, commit time as `YYYYMMDDhhmmss` UTC).
/// `.git` is removed afterwards: `dest` holds just the module tree.
pub fn checkout(url: &str, r: &GitRef, dest: &Path) -> Result<(String, String), String> {
    let _ = std::fs::remove_dir_all(dest);
    if let Some(p) = dest.parent() {
        std::fs::create_dir_all(p).map_err(|e| format!("cannot create {}: {e}", p.display()))?;
    }
    let d = dest.to_string_lossy().to_string();
    match r {
        GitRef::Tag(name) | GitRef::Branch(name) => {
            let mut c = git();
            c.args(["clone", "--quiet", "--depth", "1", "--branch", name, "--", url, &d]);
            run(c, &format!("clone {url} ({})", r.describe()))?;
        }
        GitRef::Rev(rev) => {
            std::fs::create_dir_all(dest).map_err(|e| e.to_string())?;
            let mut c = git();
            c.current_dir(dest).args(["init", "--quiet"]);
            run(c, "init")?;
            let mut c = git();
            c.current_dir(dest).args(["fetch", "--quiet", "--depth", "1", url, rev]);
            if run(c, "fetch").is_err() {
                // Servers that refuse fetching by hash: fetch everything.
                let mut c = git();
                c.current_dir(dest).args(["fetch", "--quiet", url, "+refs/heads/*:refs/remotes/origin/*", "+refs/tags/*:refs/tags/*"]);
                run(c, &format!("fetch {url}"))?;
                let mut c = git();
                c.current_dir(dest).args(["checkout", "--quiet", rev]);
                run(c, &format!("checkout {rev}"))?;
            } else {
                let mut c = git();
                c.current_dir(dest).args(["checkout", "--quiet", "FETCH_HEAD"]);
                run(c, "checkout")?;
            }
        }
    }
    let mut c = git();
    c.current_dir(dest).args(["log", "-1", "--format=%H %cd", "--date=format-local:%Y%m%d%H%M%S"]).env("TZ", "UTC");
    let out = run(c, "log")?;
    let (commit, time) = out.trim().split_once(' ').ok_or("git log: unexpected output")?;
    let (commit, time) = (commit.to_string(), time.to_string());
    std::fs::remove_dir_all(dest.join(".git")).map_err(|e| format!("cannot clean {}: {e}", dest.display()))?;
    Ok((commit, time))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn module_paths_and_urls() {
        assert_eq!(url_of("github.com/org/pkg"), "https://github.com/org/pkg");
        assert_eq!(url_of("file:///tmp/x.git"), "file:///tmp/x.git");
        assert_eq!(module_path_of("https://github.com/org/exp.git"), "github.com/org/exp");
        assert_eq!(module_path_of("git@github.com:org/exp.git"), "github.com/org/exp");
        assert_eq!(module_path_of("github.com/org/exp/"), "github.com/org/exp");
        assert_eq!(module_path_of("file:///tmp/repos/a.git"), "tmp/repos/a");
    }
}
