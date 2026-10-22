//! `workspace/willRenameFiles`: when moving a file changes the namespace
//! its path implies, the edits that keep the project compiling.
//!
//! Namespaces are not tied to paths in Slang, so edits are only computed
//! for files of a package that follow the layout convention: a file
//! `src/a/b/x.sl` of a package whose root namespace is `app` declares
//! `namespace app.a.b` (`src/x.sl`: `namespace app`). Moving such a file
//! to `src/c/x.sl` then
//! - rewrites its `namespace` line to `app.c`;
//! - when no other file declares `app.a.b` (the namespace moves as a
//!   whole), rewrites `import app.a.b` and `import app.a.b.T` everywhere;
//! - otherwise rewrites `import app.a.b.T` of the moved file's members,
//!   adds `import app.c` after `import app.a.b` in files using the moved
//!   file's declarations, and `import app.a.b` to the moved file when it
//!   uses declarations of the files left behind.
//!
//! Files outside the convention (or moved out of `src/`) get no edits.

use crate::range;
use lsp_types::{TextEdit, Url};
use slang::session::Analysis;
use slang::span::Span;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

/// The namespace a package file at `path` has by the layout convention:
/// the package's root namespace plus the directories below `src/`.
pub(crate) fn derived_namespace(path: &Path) -> Option<String> {
    let manifest = slang::package::manifest::find_upwards(path.parent()?).ok()??;
    let root = manifest.parent()?;
    let loaded = slang::package::manifest::load(&manifest).ok()?;
    let ns_root = loaded.manifest.package.as_ref()?.namespace_root();
    let rel = path.strip_prefix(root.join("src")).ok()?;
    let mut ns = vec![ns_root];
    for c in rel.parent()?.components() {
        let seg = c.as_os_str().to_str()?;
        if !crate::features::is_identifier(seg) {
            return None;
        }
        ns.push(seg.to_string());
    }
    Some(ns.join("."))
}

/// One planned move: the file's index in the analysis and the namespace
/// it gets.
struct Move {
    file: usize,
    old_ns: String,
    new_ns: String,
}

/// The edits for moving the analysed files `moves` (file index, new
/// path), by URI; empty when nothing needs to change.
pub(crate) fn rename_edits(a: &Analysis, moves: &[(usize, PathBuf)], derive: impl Fn(&Path) -> Option<String>, uri_of: impl Fn(usize) -> Option<Url>) -> HashMap<Url, Vec<TextEdit>> {
    let mut planned = Vec::new();
    for (file, new_path) in moves {
        let Some(Some(info)) = a.index.files.get(*file) else { continue };
        if a.is_read_only(*file) || derive(Path::new(&a.files[*file].path)).as_deref() != Some(info.namespace.as_str()) {
            continue;
        }
        let Some(new_ns) = derive(new_path) else { continue };
        if new_ns != info.namespace {
            planned.push(Move { file: *file, old_ns: info.namespace.clone(), new_ns });
        }
    }
    let mut edits: Vec<(usize, TextEdit)> = Vec::new();
    let mut edit = |file: usize, span: Span, text: String| {
        let e = TextEdit { range: range(&a.files[file], span), new_text: text };
        if !edits.iter().any(|(f, x)| *f == file && x.range == e.range) {
            edits.push((file, e));
        }
    };
    let moved: HashSet<usize> = planned.iter().map(|m| m.file).collect();
    let project_files = || (0..a.asts.len()).filter(|f| !a.is_read_only(*f));
    for m in &planned {
        let ast = &a.asts[m.file];
        edit(m.file, ast.namespace.span, m.new_ns.clone());
        // Does the namespace move as a whole (every file of it, to the same place)?
        let stays: Vec<usize> = project_files()
            .filter(|f| a.index.files[*f].as_ref().is_some_and(|i| i.namespace == m.old_ns))
            .filter(|f| !planned.iter().any(|p| p.file == *f && p.new_ns == m.new_ns))
            .collect();
        let whole = stays.is_empty();
        let members: HashSet<&str> = a
            .index
            .ns_members
            .get(&m.old_ns)
            .into_iter()
            .flatten()
            .filter(|(_, id)| a.index.symbol(*id).loc.is_some_and(|l| l.file == m.file))
            .map(|(n, _)| n.as_str())
            .collect();
        let uses_moved = |g: usize| a.index.references.iter().any(|r| r.file == g && a.index.symbol(r.symbol).loc.is_some_and(|l| l.file == m.file));
        for g in project_files() {
            let imports = &a.asts[g].imports;
            let has_new = imports.iter().any(|i| i.path.to_string() == m.new_ns);
            for imp in imports {
                let path = imp.path.to_string();
                if path == m.old_ns {
                    if whole {
                        edit(g, imp.path.span, m.new_ns.clone());
                    } else if !has_new && g != m.file && !moved.contains(&g) && uses_moved(g) {
                        edit(g, Span::new(imp.span.end, imp.span.end), format!("\nimport {}", m.new_ns));
                    }
                } else if let Some(rest) = path.strip_prefix(&format!("{}.", m.old_ns)) {
                    let first = rest.split('.').next().unwrap_or("");
                    if (whole && !a.index.ns_members.contains_key(&path)) || members.contains(first) {
                        edit(g, imp.path.span, format!("{}.{rest}", m.new_ns));
                    }
                }
            }
        }
        // The moved file loses implicit access to the files left behind.
        let uses_left = a.index.references.iter().any(|r| r.file == m.file && a.index.symbol(r.symbol).loc.is_some_and(|l| stays.contains(&l.file)));
        if !whole && uses_left && !ast.imports.iter().any(|i| i.path.to_string() == m.old_ns) {
            let at = ast.imports.last().map_or(ast.namespace.span.end, |i| i.span.end);
            edit(m.file, Span::new(at, at), format!("\nimport {}", m.old_ns));
        }
    }
    let mut out: HashMap<Url, Vec<TextEdit>> = HashMap::new();
    for (f, e) in edits {
        if let Some(uri) = uri_of(f) {
            out.entry(uri).or_default().push(e);
        }
    }
    out
}
