//! Version selection for module dependencies: Minimal Version Selection
//! over the whole graph (docs/PACKAGES.md).
//!
//! Every requirement (`"example.com/a" = "^1.2"`) names a module and a
//! constraint; its *minimum* is the lowest released version matching the
//! constraint. The selected version of a module is the highest minimum
//! over every requirement on it reachable from the workspace, including
//! requirements of versions that were later superseded (Go's MVS), or the
//! version in `slang.lock` when that is higher and still matches: a new
//! release changes nothing until `slang update`. Upper bounds of the
//! constraints of the final graph are then checked, and only one major
//! version of a module may appear in the graph.
//!
//! `slang update [module]` selects, for the named modules (or all), the
//! newest version matching every constraint instead.
//!
//! Git pins (`{ git, tag | branch | rev }`) select exactly their commit
//! (a pseudo-version `v0.0.0-<time>-<hash>` unless the tag is a version);
//! all pins of one module must agree.

use super::fetch::Fetcher;
use super::git::{self, GitRef};
use super::lock::LockFile;
use super::manifest::{self, Dependency, Format, Manifest};
use super::semver::{Constraint, Version};
use super::store::{self, Index};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::PathBuf;
use std::sync::Mutex;

/// What a requirement asks for.
#[derive(Clone, Debug, PartialEq)]
pub enum Spec {
    /// A version constraint, resolved from tags (proxy or git).
    Version(Constraint),
    /// A git location: an explicit pin, or a constraint over its tags.
    Git { url: String, pin: Option<GitRef>, constraint: Option<Constraint> },
}

/// One dependency of a package or module version.
#[derive(Clone, Debug)]
pub struct Requirement {
    /// Who requires it: a workspace package name or `module@version`.
    pub from: String,
    /// The key in `[dependencies]` (module path or alias).
    pub key: String,
    pub module: String,
    pub spec: Spec,
}

impl Requirement {
    fn constraint(&self) -> Option<&Constraint> {
        match &self.spec {
            Spec::Version(c) => Some(c),
            Spec::Git { constraint, pin: None, .. } => constraint.as_ref(),
            Spec::Git { .. } => None,
        }
    }

    fn describe(&self) -> String {
        match &self.spec {
            Spec::Version(c) => format!("{} requires {} {c}", self.from, self.module),
            Spec::Git { pin: Some(r), url, .. } => format!("{} requires {} at {} of {url}", self.from, self.module, r.describe()),
            Spec::Git { constraint, url, .. } => format!("{} requires {} {} from {url}", self.from, self.module, constraint.as_ref().map(|c| c.to_string()).unwrap_or_else(|| "*".into())),
        }
    }
}

/// Whether a `[dependencies]` key names a module (has a `/` or a dot in
/// its first segment: `github.com/org/pkg`) rather than an alias.
pub fn is_module_key(key: &str) -> bool {
    key.contains('/') || key.split('/').next().is_some_and(|h| h.contains('.'))
}

/// The requirement of dependency `key` of `from`; `None` for `path`
/// dependencies.
pub fn requirement(from: &str, key: &str, dep: &Dependency) -> Result<Option<Requirement>, String> {
    let (version, git, detail) = match dep {
        Dependency::Version(v) => (Some(v.clone()), None, None),
        Dependency::Detailed(d) => {
            if d.path.is_some() {
                return Ok(None);
            }
            (d.version.clone(), d.git.clone(), Some(d))
        }
    };
    let constraint = version.as_deref().map(Constraint::parse).transpose().map_err(|e| format!("dependency `{key}`: {e}"))?;
    match git {
        Some(g) => {
            let d = detail.unwrap();
            let pin = match (&d.tag, &d.branch, &d.rev) {
                (Some(t), None, None) => Some(GitRef::Tag(t.clone())),
                (None, Some(b), None) => Some(GitRef::Branch(b.clone())),
                (None, None, Some(r)) => Some(GitRef::Rev(r.clone())),
                (None, None, None) => None,
                _ => return Err(format!("dependency `{key}`: give at most one of `tag`, `branch`, `rev`")),
            };
            if pin.is_some() && constraint.is_some() {
                return Err(format!("dependency `{key}`: `version` cannot be combined with `tag`/`branch`/`rev`"));
            }
            let module = git::module_path_of(&g);
            if !store::valid_module_path(&module) {
                return Err(format!("dependency `{key}`: cannot derive a module path from git `{g}`"));
            }
            Ok(Some(Requirement { from: from.into(), key: key.into(), module, spec: Spec::Git { url: git::url_of(&g), pin, constraint } }))
        }
        None => {
            if !is_module_key(key) {
                return Err(format!(
                    "dependency `{key}`: a version needs a module path; write `\"github.com/<org>/{key}\" = \"{}\"` or `{key} = {{ git = \"github.com/<org>/{key}\", version = \"{}\" }}`",
                    version.as_deref().unwrap_or("^1"),
                    version.as_deref().unwrap_or("^1")
                ));
            }
            if !store::valid_module_path(key) {
                return Err(format!("dependency `{key}`: invalid module path"));
            }
            let c = constraint.unwrap_or_else(|| Constraint::parse("*").unwrap());
            Ok(Some(Requirement { from: from.into(), key: key.into(), module: key.into(), spec: Spec::Version(c) }))
        }
    }
}

/// Which modules `slang update` upgrades.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum Update {
    #[default]
    None,
    All,
    /// Module paths, aliases or package names.
    Only(Vec<String>),
}

/// Process-wide options of module resolution, set by the CLI.
#[derive(Clone, Debug, Default)]
pub struct Options {
    pub offline: bool,
    pub update: Update,
}

static OPTIONS: Mutex<Option<Options>> = Mutex::new(None);

pub fn set_options(o: Options) {
    *OPTIONS.lock().unwrap() = Some(o);
}

pub fn options() -> Options {
    OPTIONS.lock().unwrap().clone().unwrap_or_default()
}

/// A selected module version, ready to be materialised.
#[derive(Clone, Debug)]
pub struct Selected {
    pub module: String,
    pub version: String,
    pub h1: String,
    /// `slang.lock` source: `module`, or `git+<url>?<ref>#<commit>` for pins.
    pub source: String,
    /// Stored files (absent in vendor mode).
    pub index: Option<Index>,
    /// Directory with the module tree (set by the caller for store modules).
    pub dir: Option<PathBuf>,
    /// Requirements of this version, in manifest order.
    pub deps: Vec<Requirement>,
}

/// The manifest of a module version, parsed.
fn parse_manifest(module: &str, version: &str, name: &str, bytes: &[u8]) -> Result<Manifest, String> {
    let format = Format::of_path(std::path::Path::new(name)).ok_or_else(|| format!("{module}@{version}: unknown manifest {name}"))?;
    let text = String::from_utf8_lossy(bytes);
    let m = Manifest::parse(&text, format).map_err(|e| format!("{module}@{version}: {name}: {}", e.trim_end()))?;
    m.validate().map_err(|e| format!("{module}@{version}: {name}: {e}"))?;
    if m.package.is_none() {
        return Err(format!("{module}@{version}: {name} has no [package] section"));
    }
    Ok(m)
}

/// The requirements of a module's manifest.
pub fn module_requirements(module: &str, version: &str, m: &Manifest) -> Result<Vec<Requirement>, String> {
    let from = format!("{module}@{version}");
    let mut out = Vec::new();
    for (k, d) in &m.dependencies {
        match requirement(&from, k, d)? {
            Some(r) => out.push(r),
            None => return Err(format!("{from}: dependency `{k}` is a `path` dependency; a published module can depend only on modules")),
        }
    }
    Ok(out)
}

/// The lock's record of each module: (version, checksum, source).
fn locked_modules(lock: Option<&LockFile>) -> HashMap<String, (String, String, String)> {
    let mut out = HashMap::new();
    for p in lock.map(|l| l.packages.as_slice()).unwrap_or(&[]) {
        if let Some(m) = &p.module {
            out.insert(m.clone(), (p.version.clone(), p.checksum.clone(), p.source.clone()));
        }
    }
    out
}

fn pin_source(url: &str, r: &GitRef, commit: &str) -> String {
    let q = match r {
        GitRef::Tag(t) => format!("tag={t}"),
        GitRef::Branch(b) => format!("branch={b}"),
        GitRef::Rev(v) => format!("rev={v}"),
    };
    format!("git+{url}?{q}#{commit}")
}

/// Runs MVS from `roots`. `names` maps module paths to package names
/// already known from the lock (for `Update::Only` by package name).
pub fn resolve(roots: Vec<Requirement>, lock: Option<&LockFile>, fetcher: &Fetcher, update: &Update) -> Result<Vec<Selected>, String> {
    let locked = locked_modules(lock);
    for (m, (v, sum, _)) in &locked {
        fetcher.expect(m, v, sum);
    }
    let lock_names: HashMap<String, String> =
        lock.map(|l| l.packages.iter().filter_map(|p| p.module.clone().map(|m| (m, p.name.clone()))).collect()).unwrap_or_default();
    let upgrading = |module: &str, keys: &BTreeSet<String>| match update {
        Update::None => false,
        Update::All => true,
        Update::Only(names) => names.iter().any(|n| n == module || keys.contains(n) || lock_names.get(module) == Some(n)),
    };
    let mut reqs: Vec<Requirement> = roots.clone();
    let mut selected: BTreeMap<String, String> = BTreeMap::new();
    let mut visited: HashSet<(String, String)> = HashSet::new();
    let mut version_reqs: HashMap<(String, String), Vec<Requirement>> = HashMap::new();
    let mut pins: HashMap<(String, String, GitRef), (String, super::fetch::Fetched)> = HashMap::new();
    let mut manifests: HashMap<(String, String), Manifest> = HashMap::new();
    loop {
        let mut changed = false;
        let mut by_module: BTreeMap<String, Vec<&Requirement>> = BTreeMap::new();
        for r in &reqs {
            by_module.entry(r.module.clone()).or_default().push(r);
        }
        for (module, rs) in &by_module {
            let keys: BTreeSet<String> = rs.iter().map(|r| r.key.clone()).collect();
            let target = choose(module, rs, locked.get(module), upgrading(module, &keys), fetcher, &mut pins)?;
            if selected.get(module) != Some(&target) {
                selected.insert(module.clone(), target);
                changed = true;
            }
        }
        let todo: Vec<(String, String)> = selected.iter().map(|(m, v)| (m.clone(), v.clone())).filter(|k| !visited.contains(k)).collect();
        for (m, v) in todo {
            visited.insert((m.clone(), v.clone()));
            let url = url_override(&reqs, &m);
            let pinned = pins.iter().find(|((pm, _, _), (pv, _))| pm == &m && pv == &v).map(|(_, (_, f))| f.index.clone());
            let (name, bytes) = match pinned {
                Some(idx) => {
                    let n = idx.manifest.clone().ok_or_else(|| format!("{m}@{v} has no manifest (slang.toml, slang.json or slang.yaml)"))?;
                    let f = idx.files.iter().find(|f| f.path == n).unwrap();
                    (n, fetcher.store.read_file(f)?)
                }
                None => fetcher.manifest(&m, &v, url.as_deref())?.ok_or_else(|| format!("{m}@{v} has no manifest (slang.toml, slang.json or slang.yaml)"))?,
            };
            let man = parse_manifest(&m, &v, &name, &bytes)?;
            let rs = module_requirements(&m, &v, &man)?;
            reqs.extend(rs.iter().cloned());
            version_reqs.insert((m.clone(), v.clone()), rs);
            manifests.insert((m, v), man);
            changed = true;
        }
        if !changed {
            break;
        }
    }
    // The final graph: modules reachable from the roots through selected versions.
    let mut reach: Vec<String> = Vec::new();
    let mut active: Vec<Requirement> = roots.clone();
    let mut stack: Vec<String> = roots.iter().map(|r| r.module.clone()).collect();
    while let Some(m) = stack.pop() {
        if reach.contains(&m) {
            continue;
        }
        reach.push(m.clone());
        let v = &selected[&m];
        for r in &version_reqs[&(m.clone(), v.clone())] {
            active.push(r.clone());
            stack.push(r.module.clone());
        }
    }
    reach.sort();
    // Upper bounds of the active constraints and the one-major rule.
    for m in &reach {
        let v = &selected[m];
        let ver = Version::parse(v);
        let on: Vec<&Requirement> = active.iter().filter(|r| &r.module == m).collect();
        for r in &on {
            if let (Some(c), Some(ver)) = (r.constraint(), &ver) {
                if !c.matches(ver) {
                    let why: Vec<String> = on.iter().map(|r| format!("  {}", r.describe())).collect();
                    return Err(format!("no version of {m} satisfies every requirement (selected {v}):\n{}", why.join("\n")));
                }
            }
        }
    }
    let mut out = Vec::new();
    for m in reach {
        let v = selected[&m].clone();
        let deps = version_reqs[&(m.clone(), v.clone())].clone();
        let pin = pins.iter().find(|((pm, _, _), (pv, _))| pm == &m && pv == &v).map(|((_, url, r), (_, f))| (url.clone(), r.clone(), f.clone()));
        let (index, source) = match pin {
            Some((url, r, f)) => {
                let commit = f.commit.clone().unwrap_or_default();
                (f.index, pin_source(&url, &r, &commit))
            }
            None => (fetcher.download(&m, &v, url_override(&active, &m).as_deref())?.index, "module".to_string()),
        };
        if let Some((lv, lsum, _)) = locked.get(&m) {
            if lv == &v && lsum != &index.h1 {
                return Err(format!(
                    "SECURITY ERROR: checksum mismatch for {m}@{v}\n  downloaded: {}\n  slang.lock: {lsum}\nthe module content differs from the locked one; it may have been tampered with",
                    index.h1
                ));
            }
        }
        out.push(Selected { module: m, version: v, h1: index.h1.clone(), source, index: Some(index), dir: None, deps });
    }
    Ok(out)
}

/// The git location given for `module` by an alias requirement, if any.
fn url_override(reqs: &[Requirement], module: &str) -> Option<String> {
    reqs.iter().find_map(|r| match &r.spec {
        Spec::Git { url, pin: None, .. } if r.module == module => Some(url.clone()),
        _ => None,
    })
}

/// The version of `module` the requirements `rs` select.
fn choose(
    module: &str,
    rs: &[&Requirement],
    locked: Option<&(String, String, String)>,
    upgrade: bool,
    fetcher: &Fetcher,
    pins: &mut HashMap<(String, String, GitRef), (String, super::fetch::Fetched)>,
) -> Result<String, String> {
    let pinned: Vec<(&String, &GitRef, &Requirement)> = rs
        .iter()
        .filter_map(|r| match &r.spec {
            Spec::Git { url, pin: Some(p), .. } => Some((url, p, *r)),
            _ => None,
        })
        .collect();
    if let Some((url, r, _)) = pinned.first() {
        if let Some((_, _, other)) = pinned.iter().find(|(u, p, _)| u != url || p != r) {
            return Err(format!("conflicting git pins of {module}:\n  {}\n  {}\n(use the same `git` and `tag`/`branch`/`rev` everywhere)", pinned[0].2.describe(), other.describe()));
        }
        let key = (module.to_string(), url.to_string(), (*r).clone());
        if let Some((v, _)) = pins.get(&key) {
            return Ok(v.clone());
        }
        // Reuse the locked commit only for the same location and ref.
        let want = pin_source(url, r, "");
        let lockpin = locked.filter(|_| !upgrade).and_then(|(v, _, s)| {
            let (loc, commit) = s.rsplit_once('#')?;
            (format!("{loc}#") == want).then_some((v.as_str(), commit))
        });
        let (v, f) = fetcher.git_pin(module, url, r, lockpin)?;
        pins.insert(key, (v.clone(), f));
        return Ok(v);
    }
    let cons: Vec<(&Constraint, &Requirement)> = rs.iter().filter_map(|r| r.constraint().map(|c| (c, *r))).collect();
    // One major version per graph.
    let mut majors: Vec<(u64, &Requirement)> = cons.iter().filter_map(|(c, r)| c.major().map(|m| (m, *r))).collect();
    majors.sort_by_key(|(m, _)| *m);
    if let (Some(lo), Some(hi)) = (majors.first(), majors.last()) {
        if lo.0 != hi.0 {
            return Err(format!(
                "{module}: only one major version of a module may be in the graph, but\n  {}\n  {}\nupdate the older requirement (a different module path is a different module)",
                lo.1.describe(),
                hi.1.describe()
            ));
        }
    }
    if let Some((lv, _, _)) = locked {
        if let Some(l) = Version::parse(lv) {
            if !upgrade && cons.iter().all(|(c, _)| c.matches(&l)) {
                return Ok(lv.clone());
            }
        }
    }
    let url = rs.iter().find_map(|r| match &r.spec {
        Spec::Git { url, pin: None, .. } => Some(url.clone()),
        _ => None,
    });
    let versions = fetcher.versions(module, url.as_deref())?;
    let shown = || {
        let v: Vec<String> = versions.iter().rev().take(8).map(|v| v.tag()).collect();
        if v.is_empty() {
            "none (no version tags)".to_string()
        } else {
            v.join(", ")
        }
    };
    if upgrade {
        let best = versions.iter().filter(|v| cons.iter().all(|(c, _)| c.matches(v))).max();
        if let Some(b) = best {
            return Ok(b.tag());
        }
    }
    let mut best: Option<&Version> = None;
    for (c, r) in &cons {
        let m = c.lowest(&versions).ok_or_else(|| format!("no version of {module} matches `{c}` ({}); available: {}", r.describe(), shown()))?;
        if best.is_none_or(|b| m > b) {
            best = Some(m);
        }
    }
    match best {
        Some(b) => Ok(b.tag()),
        None => versions.iter().filter(|v| !v.is_prerelease()).max().map(|v| v.tag()).ok_or_else(|| format!("{module} has no released versions; available: {}", shown())),
    }
}

/// Vendor mode: the modules of `lock`, read from `vendor/<module>@<version>/`
/// (each checked against its lock checksum).
pub fn from_vendor(vendor: &std::path::Path, lock: &LockFile, roots: &[Requirement]) -> Result<Vec<Selected>, String> {
    let locked = locked_modules(Some(lock));
    let mut out = Vec::new();
    let mut by_module: HashMap<String, (String, String, String)> = HashMap::new();
    for (m, rec) in &locked {
        by_module.insert(m.clone(), rec.clone());
    }
    let mut stack: Vec<Requirement> = roots.to_vec();
    let mut seen = HashSet::new();
    while let Some(r) = stack.pop() {
        let (v, sum, source) = by_module.get(&r.module).cloned().ok_or_else(|| format!("{} ({}) is not in slang.lock; vendor/ is out of date, run `slang vendor`", r.module, r.describe()))?;
        if let (Some(c), Some(ver)) = (r.constraint(), Version::parse(&v)) {
            if !c.matches(&ver) {
                return Err(format!("{}: locked {v} does not match `{c}` ({}); run `slang update` and `slang vendor`", r.module, r.describe()));
            }
        }
        if !seen.insert(r.module.clone()) {
            continue;
        }
        let dir = vendor.join(format!("{}@{v}", store::escape_path(&r.module)));
        let files = super::sum::read_tree(&dir).map_err(|e| format!("vendor/: {e}; run `slang vendor`"))?;
        let h = super::sum::h1(&files);
        if h != sum {
            return Err(format!("vendor/{}@{v}: checksum mismatch (vendor {h}, slang.lock {sum}); run `slang vendor`", r.module));
        }
        let (name, bytes) = super::fetch::manifest_entry(&files).ok_or_else(|| format!("vendor/{}@{v} has no manifest", r.module))?;
        let man = parse_manifest(&r.module, &v, &name, &bytes)?;
        let deps = module_requirements(&r.module, &v, &man)?;
        stack.extend(deps.iter().cloned());
        out.push(Selected { module: r.module.clone(), version: v, h1: h, source, index: None, dir: Some(dir), deps });
    }
    out.sort_by(|a, b| a.module.cmp(&b.module));
    Ok(out)
}

/// The manifest names a module tree may use (for error messages).
pub fn manifest_names() -> String {
    manifest::MANIFEST_NAMES.join(", ")
}
