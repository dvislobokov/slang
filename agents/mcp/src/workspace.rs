//! The project the server works on: its `.zl` files loaded from disk
//! into a [`Session`] (re-read before every analysis, so agents' edits
//! are always seen), dependency sources from `.zell/deps/` or `vendor/`
//! (read-only) and the std index, plus position and path helpers.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use zell::session::{Analysis, Session};
use zell::span::Span;
use zell::stdindex::StdIndex;

/// Manifest file names that mark a project root (docs/PACKAGES.md).
pub const MANIFESTS: &[&str] = &["zell.toml", "zell.json", "zell.yaml", "zell.yml"];

/// Upper bound on files loaded from one root.
const MAX_FILES: usize = 5000;

/// The loaded project.
pub struct Workspace {
    /// The project root (canonical when it exists).
    pub root: PathBuf,
    session: Session,
    /// Directories whose `.zl` files are loaded.
    roots: Vec<PathBuf>,
    /// Single files outside every root that a tool asked for.
    extra: BTreeSet<PathBuf>,
    std: Option<Option<Arc<StdIndex>>>,
    analysis: Option<Arc<Analysis>>,
}

fn has_manifest(dir: &Path) -> bool {
    MANIFESTS.iter().any(|m| dir.join(m).is_file())
}

/// Every `.zl` file under `dir` (skipping hidden directories, `target`,
/// `node_modules` and `out`, as the language server does).
pub(crate) fn collect(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for e in entries.flatten() {
        if out.len() >= MAX_FILES {
            return;
        }
        let p = e.path();
        let name = e.file_name().to_string_lossy().to_string();
        if p.is_dir() {
            if !name.starts_with('.') && !matches!(name.as_str(), "target" | "node_modules" | "out") {
                collect(&p, out);
            }
        } else if p.extension().is_some_and(|x| x == "zl") {
            out.push(p);
        }
    }
}

impl Workspace {
    pub fn new(root: PathBuf) -> Workspace {
        let root = root.canonicalize().unwrap_or(root);
        Workspace { roots: vec![root.clone()], root, session: Session::new(), extra: BTreeSet::new(), std: None, analysis: None }
    }

    /// The nearest directory with a manifest above the current one, else
    /// the current directory.
    pub fn default_root() -> PathBuf {
        let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
        cwd.ancestors().find(|d| has_manifest(d)).map(Path::to_path_buf).unwrap_or(cwd)
    }

    /// The std declaration index (loaded once).
    pub fn std_index(&mut self) -> Option<Arc<StdIndex>> {
        self.std.get_or_insert_with(|| zell::sema::stdfs::std_dir().map(|d| Arc::new(StdIndex::load(&d)))).clone()
    }

    /// The files a root contributes: `src/**` when it has a manifest and
    /// `src/`, else the whole tree; plus its locked dependencies
    /// (read-only).
    fn root_files(root: &Path, out: &mut BTreeMap<PathBuf, bool>) {
        let src = root.join("src");
        let manifest = has_manifest(root);
        let mut files = Vec::new();
        collect(if manifest && src.is_dir() { &src } else { root }, &mut files);
        for f in files {
            out.entry(f).or_insert(false);
        }
        if !manifest {
            return;
        }
        let Some(lock) = std::fs::read_to_string(root.join("zell.lock")).ok().and_then(|t| zell::package::lock::LockFile::parse(&t).ok()) else { return };
        let vendored = root.join("vendor/modules.yaml").is_file();
        for p in &lock.packages {
            let Some(module) = &p.module else { continue };
            let name = format!("{}@{}", zell::package::store::escape_path(module), p.version);
            let dir = if vendored { root.join("vendor").join(&name) } else { root.join(".zell/deps").join(&name) };
            let mut files = Vec::new();
            collect(&dir.join("src"), &mut files);
            for f in files {
                out.insert(f, true);
            }
        }
    }

    /// Re-reads the project from disk into the session; returns whether
    /// anything changed.
    fn refresh(&mut self) -> bool {
        let std = self.std_index();
        self.session.set_std_index(std);
        let mut wanted: BTreeMap<PathBuf, bool> = BTreeMap::new();
        for r in &self.roots {
            Self::root_files(r, &mut wanted);
        }
        for f in &self.extra {
            wanted.entry(f.clone()).or_insert(false);
        }
        let wanted: BTreeMap<String, (PathBuf, bool)> = wanted.into_iter().map(|(p, ro)| (p.display().to_string(), (p, ro))).collect();
        let mut changed = false;
        let stale: Vec<String> = self.session.paths().filter(|p| !wanted.contains_key(*p)).map(str::to_string).collect();
        for p in stale {
            self.session.remove_file(&p);
            changed = true;
        }
        for (key, (path, ro)) in &wanted {
            let Ok(text) = std::fs::read_to_string(path) else { continue };
            if self.session.text(key) != Some(text.as_str()) {
                self.session.update_file(key.clone(), text);
                self.session.set_read_only(key, *ro);
                changed = true;
            }
        }
        changed
    }

    /// An analysis of the files as they are on disk now.
    pub fn analysis(&mut self) -> Arc<Analysis> {
        if self.refresh() || self.analysis.is_none() {
            self.analysis = Some(Arc::new(self.session.analyze()));
        }
        self.analysis.clone().expect("analysed")
    }

    /// The absolute path of `path` (relative to the root).
    pub fn abs(&self, path: &str) -> PathBuf {
        let p = Path::new(path);
        let p = if p.is_absolute() { p.to_path_buf() } else { self.root.join(p) };
        p.canonicalize().unwrap_or(p)
    }

    /// Makes sure `path` takes part in the analysis: a file outside the
    /// loaded roots brings in its project (found by its manifest), or
    /// itself.
    fn include(&mut self, path: &Path) {
        if self.roots.iter().any(|r| path.starts_with(r)) {
            return;
        }
        match path.parent().and_then(|d| d.ancestors().find(|a| has_manifest(a))) {
            Some(project) => self.roots.push(project.to_path_buf()),
            None => {
                self.extra.insert(path.to_path_buf());
            }
        }
    }

    /// The analysis and the index of file `path` in it.
    pub fn file(&mut self, path: &str) -> Result<(Arc<Analysis>, usize), String> {
        let abs = self.abs(path);
        if !abs.is_file() {
            return Err(format!("no such file: {path}"));
        }
        if abs.extension().is_none_or(|x| x != "zl") {
            return Err(format!("{path} is not a Zell source file (.zl)"));
        }
        self.include(&abs);
        let a = self.analysis();
        let key = abs.display().to_string();
        let i = a.file_index(&key).ok_or_else(|| format!("{path} is not part of the analysed project"))?;
        Ok((a, i))
    }

    /// The project's own files of an analysis (not std or dependencies).
    pub fn project_files(&self, a: &Analysis) -> Vec<usize> {
        (0..a.asts.len()).filter(|&f| !a.is_read_only(f)).collect()
    }

    /// `path` for display: relative to the root when inside it.
    pub fn rel(&self, path: &str) -> String {
        match Path::new(path).strip_prefix(&self.root) {
            Ok(r) if !r.as_os_str().is_empty() => r.display().to_string(),
            _ => path.to_string(),
        }
    }

    /// `path:line:col` (1-based, columns in characters).
    pub fn loc(&self, a: &Analysis, file: usize, span: Span) -> String {
        let f = &a.files[file];
        let (l, c) = f.line_col(span.start);
        format!("{}:{l}:{c}", self.rel(&f.path))
    }

    /// The source line of `span`'s start, trimmed.
    pub fn line_of(a: &Analysis, file: usize, span: Span) -> String {
        let f = &a.files[file];
        let (l, _) = f.line_col(span.start);
        f.line_text(l).trim().to_string()
    }
}

/// The byte offset of 1-based `line` and character column `col` in
/// `text` (columns past the end of the line clamp to it).
pub fn offset_of(f: &zell::span::SourceFile, line: usize, col: usize) -> Result<u32, String> {
    if line == 0 || line > f.line_count() {
        return Err(format!("line {line} is out of range (the file has {} lines)", f.line_count()));
    }
    let start = f.line_start(line - 1) as usize;
    let text = f.line_text(line);
    let byte = text.char_indices().nth(col.saturating_sub(1)).map_or(text.len(), |(i, _)| i);
    Ok((start + byte) as u32)
}
