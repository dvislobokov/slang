//! Project discovery and the package graph: workspace members, `path`
//! dependencies (resolved recursively, deduplicated by canonical path),
//! cycle detection and namespace-root ownership.

use super::manifest::{self, Dependency, LoadedManifest};
use crate::sema::{ImportPolicy, PackageScope};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

/// Index of a package in [`Project::packages`].
pub type PkgId = usize;

/// Where a package comes from. Stage 1 knows only local paths; registry
/// and git sources slot in here (and in [`Source::lock_id`]).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Source {
    /// A local directory, relative to the workspace root (`/`-separated;
    /// `.` for the root itself; absolute if outside the root).
    Path(String),
}

impl Source {
    /// The `source` field of `slang.lock`.
    pub fn lock_id(&self) -> String {
        match self {
            Source::Path(p) => format!("path:{p}"),
        }
    }
}

/// A package of the graph.
#[derive(Clone, Debug)]
pub struct PackageNode {
    pub name: String,
    pub version: String,
    /// Root namespace owned by the package.
    pub namespace: String,
    /// Canonical package directory.
    pub dir: PathBuf,
    pub manifest: LoadedManifest,
    pub source: Source,
    /// Direct `[dependencies]`.
    pub deps: Vec<PkgId>,
    /// Direct `[dev-dependencies]` (resolved for workspace members only).
    pub dev_deps: Vec<PkgId>,
    /// A workspace member (or the single root package).
    pub member: bool,
}

impl PackageNode {
    /// Entry file of a binary package; `None` for a library.
    pub fn entry(&self) -> Option<PathBuf> {
        let p = self.manifest.manifest.package.as_ref()?;
        p.entry.as_ref().map(|e| self.dir.join(e))
    }

    pub fn is_bin(&self) -> bool {
        self.entry().is_some()
    }

    /// Source files: `src/**/*.sl` plus the entry file, sorted.
    pub fn source_files(&self) -> Result<Vec<PathBuf>, String> {
        let src = self.dir.join("src");
        let mut files = if src.is_dir() { crate::driver::collect_files(&src).map_err(|e| format!("cannot read {}: {e}", src.display()))? } else { Vec::new() };
        if let Some(e) = self.entry() {
            if !e.is_file() {
                return Err(format!("entry file {} of package `{}` does not exist", e.display(), self.name));
            }
            let e = e.canonicalize().unwrap_or(e);
            if !files.iter().any(|f| f.canonicalize().ok().as_ref() == Some(&e)) {
                files.push(e);
                files.sort();
            }
        }
        Ok(files)
    }
}

/// A discovered project: the root manifest and the whole package graph.
#[derive(Clone, Debug)]
pub struct Project {
    /// Canonical directory of the root (workspace) manifest; `slang.lock`
    /// and `target/` live here.
    pub root: PathBuf,
    pub root_manifest: LoadedManifest,
    pub packages: Vec<PackageNode>,
    /// Workspace members (including the root package, if any).
    pub members: Vec<PkgId>,
    /// The root manifest's own package, if it has `[package]`.
    pub root_package: Option<PkgId>,
    /// The member nearest to the directory the project was discovered
    /// from (what a plain `slang build` builds), if any.
    pub current: Option<PkgId>,
}

impl Project {
    /// Finds the project containing `start` (a directory): the nearest
    /// manifest, or an enclosing workspace that lists it as a member.
    /// `Ok(None)` if there is no manifest up to the filesystem root.
    pub fn discover(start: &Path) -> Result<Option<Project>, String> {
        let start = start.canonicalize().map_err(|e| format!("cannot read {}: {e}", start.display()))?;
        let Some(nearest) = manifest::find_upwards(&start)? else { return Ok(None) };
        let nearest = manifest::load(&nearest)?;
        let mut root = nearest.clone();
        if nearest.manifest.workspace.is_none() {
            let mut dir = nearest.dir().parent();
            while let Some(d) = dir {
                if let Some(p) = manifest::manifest_in(d)? {
                    let lm = manifest::load(&p)?;
                    if lm.manifest.workspace.is_some() && member_dirs(&lm)?.iter().any(|m| m == nearest.dir()) {
                        root = lm;
                        break;
                    }
                }
                dir = d.parent();
            }
        }
        let mut project = Project::load(root)?;
        project.current = project.packages.iter().position(|p| p.dir == nearest.dir() && p.member);
        Ok(Some(project))
    }

    /// Loads the graph rooted at manifest `root`.
    pub fn load(root: LoadedManifest) -> Result<Project, String> {
        let root_dir = root.dir().to_path_buf();
        let mut b = Builder { root: root_dir.clone(), packages: Vec::new(), by_dir: HashMap::new(), stack: Vec::new() };
        let mut members = Vec::new();
        let mut root_package = None;
        if root.manifest.package.is_some() {
            let id = b.visit(&root_dir, true)?;
            root_package = Some(id);
            members.push(id);
        }
        if root.manifest.workspace.is_some() {
            for d in member_dirs(&root)? {
                let id = b.visit(&d, true)?;
                if !members.contains(&id) {
                    members.push(id);
                }
            }
        }
        // Dev-dependencies of members (outside the cycle check: a test kit
        // may depend on the package it tests).
        for &m in &members {
            let deps = b.packages[m].manifest.manifest.dev_dependencies.clone();
            let mut ids = Vec::new();
            for (name, dep) in &deps {
                ids.push(b.dep(m, name, dep, "dev-dependencies")?);
            }
            b.packages[m].dev_deps = ids;
        }
        let packages = b.packages;
        // Namespace roots and names are unique in the graph.
        for (i, p) in packages.iter().enumerate() {
            for q in &packages[..i] {
                if q.namespace == p.namespace {
                    return Err(format!(
                        "packages `{}` ({}) and `{}` ({}) both own the namespace root `{}`; every package in the graph needs its own root (set `namespace` in [package])",
                        q.name,
                        q.manifest.path.display(),
                        p.name,
                        p.manifest.path.display(),
                        p.namespace
                    ));
                }
                if q.name == p.name {
                    return Err(format!("two different packages are named `{}`: {} and {}", p.name, q.manifest.path.display(), p.manifest.path.display()));
                }
            }
        }
        Ok(Project { root: root_dir, root_manifest: root, packages, members, root_package, current: None })
    }

    pub fn find_member(&self, name: &str) -> Option<PkgId> {
        self.members.iter().copied().find(|&m| self.packages[m].name == name)
    }

    /// `pkg` and everything it depends on (dev-dependencies of `pkg` too
    /// when `dev`), dependencies first; the order is deterministic.
    pub fn closure(&self, pkg: PkgId, dev: bool) -> Vec<PkgId> {
        fn go(p: &Project, id: PkgId, seen: &mut Vec<bool>, out: &mut Vec<PkgId>) {
            if std::mem::replace(&mut seen[id], true) {
                return;
            }
            for &d in &p.packages[id].deps {
                go(p, d, seen, out);
            }
            out.push(id);
        }
        let mut seen = vec![false; self.packages.len()];
        let mut out = Vec::new();
        if dev {
            seen[pkg] = true;
            for &d in &self.packages[pkg].dev_deps {
                go(self, d, &mut seen, &mut out);
            }
            seen[pkg] = false;
        }
        go(self, pkg, &mut seen, &mut out);
        out
    }

    /// The isolation policy of a build of `closure` (see [`Project::closure`])
    /// whose files belong to `file_pkg` (positions in `closure`). With
    /// `dev`, the last package (the member) also sees its dev-dependencies.
    pub fn policy(&self, closure: &[PkgId], file_pkg: Vec<usize>, dev: bool) -> ImportPolicy {
        let local = |id: PkgId| closure.iter().position(|&c| c == id);
        let packages = closure
            .iter()
            .enumerate()
            .map(|(i, &id)| {
                let p = &self.packages[id];
                let mut deps: Vec<usize> = p.deps.iter().filter_map(|&d| local(d)).collect();
                if dev && i + 1 == closure.len() {
                    deps.extend(p.dev_deps.iter().filter_map(|&d| local(d)));
                }
                PackageScope { name: p.name.clone(), namespace: p.namespace.clone(), deps }
            })
            .collect();
        ImportPolicy { file_package: file_pkg, packages }
    }

    /// Directory of build outputs and the object cache.
    pub fn target_dir(&self) -> PathBuf {
        self.root.join("target")
    }

    pub fn lock_path(&self) -> PathBuf {
        self.root.join("slang.lock")
    }
}

/// Member directories of a workspace manifest (globs expanded, sorted;
/// glob matches without a manifest are skipped, literal members must have one).
pub fn member_dirs(lm: &LoadedManifest) -> Result<Vec<PathBuf>, String> {
    let Some(ws) = &lm.manifest.workspace else { return Ok(Vec::new()) };
    let mut out = Vec::new();
    for m in &ws.members {
        let is_glob = m.contains(['*', '?', '[']);
        let pattern = lm.dir().join(m);
        let mut found: Vec<PathBuf> = if is_glob {
            let pat = pattern.to_string_lossy().to_string();
            glob::glob(&pat).map_err(|e| format!("{}: bad workspace member pattern `{m}`: {e}", lm.path.display()))?.filter_map(Result::ok).filter(|p| p.is_dir()).collect()
        } else {
            if !pattern.is_dir() {
                return Err(format!("{}: workspace member `{m}` is not a directory", lm.path.display()));
            }
            vec![pattern]
        };
        found.sort();
        for d in found {
            if manifest::manifest_in(&d)?.is_none() {
                if is_glob {
                    continue;
                }
                return Err(format!("{}: workspace member `{m}` has no manifest", lm.path.display()));
            }
            let d = d.canonicalize().map_err(|e| format!("cannot read {}: {e}", d.display()))?;
            if !out.contains(&d) {
                out.push(d);
            }
        }
    }
    Ok(out)
}

/// `to` relative to `from` (both canonical), `/`-separated, with `..`
/// where needed; `.` when equal. Keeps the lock file machine-independent.
fn relative_path(from: &Path, to: &Path) -> String {
    let a: Vec<_> = from.components().collect();
    let b: Vec<_> = to.components().collect();
    let common = a.iter().zip(&b).take_while(|(x, y)| x == y).count();
    let mut parts: Vec<String> = vec!["..".to_string(); a.len() - common];
    parts.extend(b[common..].iter().map(|c| c.as_os_str().to_string_lossy().to_string()));
    if parts.is_empty() {
        ".".to_string()
    } else {
        parts.join("/")
    }
}

struct Builder {
    root: PathBuf,
    packages: Vec<PackageNode>,
    by_dir: HashMap<PathBuf, PkgId>,
    /// Packages whose dependencies are being resolved (cycle detection).
    stack: Vec<PkgId>,
}

impl Builder {
    /// Adds the package in `dir` and, recursively, its dependencies.
    fn visit(&mut self, dir: &Path, member: bool) -> Result<PkgId, String> {
        let dir = dir.canonicalize().map_err(|e| format!("cannot read {}: {e}", dir.display()))?;
        if let Some(&id) = self.by_dir.get(&dir) {
            if let Some(pos) = self.stack.iter().position(|&s| s == id) {
                let mut chain: Vec<&str> = self.stack[pos..].iter().map(|&s| self.packages[s].name.as_str()).collect();
                chain.push(&self.packages[id].name);
                return Err(format!("dependency cycle: {}", chain.join(" -> ")));
            }
            self.packages[id].member |= member;
            return Ok(id);
        }
        let lm = manifest::load_dir(&dir)?;
        let Some(pkg) = lm.manifest.package.clone() else {
            return Err(format!("{} has no [package] section (a virtual workspace cannot be a dependency)", lm.path.display()));
        };
        let id = self.packages.len();
        let rel = relative_path(&self.root, &dir);
        self.packages.push(PackageNode {
            name: pkg.name.clone(),
            version: pkg.version.clone(),
            namespace: pkg.namespace_root(),
            dir: dir.clone(),
            manifest: lm.clone(),
            source: Source::Path(rel),
            deps: Vec::new(),
            dev_deps: Vec::new(),
            member,
        });
        self.by_dir.insert(dir, id);
        self.stack.push(id);
        let mut deps = Vec::new();
        for (name, dep) in &lm.manifest.dependencies {
            deps.push(self.dep(id, name, dep, "dependencies")?);
        }
        self.stack.pop();
        self.packages[id].deps = deps;
        Ok(id)
    }

    /// Resolves dependency `name` of package `from`.
    fn dep(&mut self, from: PkgId, name: &str, dep: &Dependency, section: &str) -> Result<PkgId, String> {
        let owner = self.packages[from].name.clone();
        let mpath = self.packages[from].manifest.path.clone();
        let Some(path) = dep.path() else {
            let kind = match dep {
                Dependency::Detailed(d) if d.git.is_some() => "git dependencies",
                _ => "registry dependencies",
            };
            return Err(format!("{}: [{section}] {name}: {kind} are not supported yet (stage 1 supports only `path` dependencies)", mpath.display()));
        };
        let dir = self.packages[from].dir.join(path);
        if !dir.is_dir() {
            return Err(format!("{}: [{section}] {name}: path `{path}` is not a directory", mpath.display()));
        }
        let id = self.visit(&dir, false)?;
        let real = &self.packages[id].name;
        if real != name {
            return Err(format!("{}: [{section}] {name}: the package at `{path}` is named `{real}`; use `{real} = {{ path = \"{path}\" }}`", mpath.display()));
        }
        if id == from {
            return Err(format!("package `{owner}` depends on itself"));
        }
        let have = &self.packages[id].manifest.manifest.features;
        for f in dep.features() {
            if !have.contains_key(f.as_str()) {
                return Err(format!("{}: [{section}] {name}: package `{name}` has no feature `{f}`", mpath.display()));
            }
        }
        // `dep/feature` entries of our own [features] table.
        for (feat, items) in &self.packages[from].manifest.manifest.features {
            for it in items {
                if let Some((d, f)) = it.split_once('/') {
                    if d == name && !have.contains_key(f) {
                        return Err(format!("{}: feature `{feat}` enables `{it}`, but package `{name}` has no feature `{f}`", mpath.display()));
                    }
                }
            }
        }
        Ok(id)
    }
}
