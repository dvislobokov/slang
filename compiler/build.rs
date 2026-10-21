//! Writes the repository's `std/` tree (docs/TOOLING.md, "Исходники
//! стандартной библиотеки и зависимостей для IDE"; layout in
//! `src/sema/stdfs.rs`):
//!
//! * every `src/sema/stdlib_src/*.sl` and `src/sema/reflect_src/*.sl`, and
//!   the `std.collections` source of `src/sema/collections.rs`, copied to
//!   `std/<namespace path>/<file>.sl`;
//! * for every namespace with rows in the `FUNCS` table of
//!   `src/sema/stdlib.rs`, `std/<namespace path>/<last segment>_extern.sl`
//!   with one `extern func` declaration per row and its doc comment (the
//!   row's own doc or `src/sema/stdlib_docs.rs`).
//!
//! Files are only rewritten when their contents change, and `.sl` files
//! of `std/` that are no longer generated are removed.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

const KEYWORDS: &[&str] = &[
    "namespace", "import", "as", "struct", "interface", "enum", "func", "static", "async", "await", "var", "const", "if", "else", "for", "in", "switch", "case", "default", "return", "break", "continue",
    "defer", "try", "catch", "new", "nil", "true", "false", "map", "this",
];

/// The string literals of one line of Rust source, in order.
fn literals(line: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut chars = line.chars();
    while let Some(c) = chars.next() {
        if c != '"' {
            continue;
        }
        let mut s = String::new();
        while let Some(c) = chars.next() {
            match c {
                '\\' => match chars.next() {
                    Some('n') => s.push('\n'),
                    Some('t') => s.push('\t'),
                    Some(o) => s.push(o),
                    None => {}
                },
                '"' => break,
                _ => s.push(c),
            }
        }
        out.push(s);
    }
    out
}

fn namespace_of(src: &str) -> Option<String> {
    src.lines().find_map(|l| l.trim().strip_prefix("namespace ").map(|n| n.trim().to_string()))
}

fn rel_dir(ns: &str) -> PathBuf {
    ns.split('.').skip(1).collect()
}

/// Slang spelling of a type of the table's mini syntax (`func(a;b) r`).
fn slang_type(t: &str) -> String {
    t.trim().replace(';', ", ")
}

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let sema = manifest.join("src/sema");
    for p in ["stdlib_src", "reflect_src", "stdlib.rs", "stdlib_docs.rs", "collections.rs"] {
        println!("cargo:rerun-if-changed=src/sema/{p}");
    }
    println!("cargo:rerun-if-changed=build.rs");
    let out = manifest.parent().unwrap().join("std");
    let mut files: BTreeMap<PathBuf, String> = BTreeMap::new();

    for dir in ["stdlib_src", "reflect_src"] {
        let Ok(rd) = std::fs::read_dir(sema.join(dir)) else { continue };
        for e in rd.flatten() {
            let p = e.path();
            if p.extension().is_none_or(|x| x != "sl") {
                continue;
            }
            let Ok(src) = std::fs::read_to_string(&p) else { continue };
            let Some(ns) = namespace_of(&src) else { continue };
            files.insert(rel_dir(&ns).join(p.file_name().unwrap()), src);
        }
    }
    if let Ok(c) = std::fs::read_to_string(sema.join("collections.rs")) {
        if let Some(start) = c.find("const SOURCE: &str = r#\"") {
            let body = &c[start + "const SOURCE: &str = r#\"".len()..];
            if let Some(end) = body.find("\"#;") {
                files.insert(PathBuf::from("collections/collections.sl"), body[..end].to_string());
            }
        }
    }

    // Documentation table: path -> (parameter names, doc).
    let mut docs: BTreeMap<String, (String, String)> = BTreeMap::new();
    if let Ok(d) = std::fs::read_to_string(sema.join("stdlib_docs.rs")) {
        for line in d.lines().filter(|l| l.trim_start().starts_with("(\"std")) {
            let l = literals(line);
            if l.len() == 3 {
                docs.insert(l[0].clone(), (l[1].clone(), l[2].clone()));
            }
        }
    }
    // FUNCS rows: f!(ns, name, params, ret, symbol[, doc]).
    let mut by_ns: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let table = std::fs::read_to_string(sema.join("stdlib.rs")).unwrap_or_default();
    let mut seen = BTreeSet::new();
    for line in table.lines().filter(|l| l.trim_start().starts_with("f!(")) {
        let l = literals(line);
        if l.len() < 5 {
            continue;
        }
        let (ns, name, params, ret, sym) = (&l[0], &l[1], &l[2], &l[3], &l[4]);
        if !seen.insert(format!("{ns}.{name}")) {
            continue;
        }
        let path = format!("{ns}.{name}");
        let (names, doc) = match (l.get(5), docs.get(&path)) {
            (Some(d), table) => (table.map(|t| t.0.clone()).unwrap_or_default(), d.clone()),
            (None, Some((n, d))) => (n.clone(), d.clone()),
            (None, None) => (String::new(), String::new()),
        };
        let types: Vec<&str> = if params.trim().is_empty() { Vec::new() } else { params.split(", ").collect() };
        let mut pnames: Vec<String> = names.split(',').map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).collect();
        if pnames.len() != types.len() {
            pnames = (0..types.len()).map(|i| ((b'a' + (i % 26) as u8) as char).to_string()).collect();
        }
        let ps: Vec<String> = pnames
            .iter()
            .zip(&types)
            .map(|(n, t)| {
                let n = if KEYWORDS.contains(&n.as_str()) { format!("{n}_") } else { n.clone() };
                format!("{n} {}", slang_type(t))
            })
            .collect();
        let ret = if ret.trim() == "void" { String::new() } else { format!(" {}", slang_type(ret)) };
        let mut decl = String::new();
        if doc.is_empty() {
            let internal = name.starts_with("rt") && name[2..].starts_with(|c: char| c.is_ascii_uppercase());
            decl.push_str(if internal { "// Internal runtime primitive (used by the Slang sources of this namespace).\n" } else { "" });
        } else {
            for dl in doc.lines() {
                decl.push_str(&format!("// {dl}\n"));
            }
            decl.push_str("//\n");
        }
        decl.push_str(&format!("// C: {sym} (runtime/src/std_*.c).\nextern func {name}({}){ret}\n", ps.join(", ")));
        by_ns.entry(ns.clone()).or_default().push(decl);
    }
    for (ns, decls) in by_ns {
        let last = ns.rsplit('.').next().unwrap();
        let mut text = format!(
            "// Code generated by compiler/build.rs from compiler/src/sema/stdlib.rs (FUNCS) and\n// stdlib_docs.rs; DO NOT EDIT. The functions of {ns} implemented by the C runtime.\nnamespace {ns}\n"
        );
        for d in decls {
            text.push('\n');
            text.push_str(&d);
        }
        files.insert(rel_dir(&ns).join(format!("{last}_extern.sl")), text);
    }

    // Write what changed, remove what is no longer generated.
    for (rel, text) in &files {
        let p = out.join(rel);
        if std::fs::read_to_string(&p).ok().as_deref() == Some(text.as_str()) {
            continue;
        }
        if let Some(d) = p.parent() {
            let _ = std::fs::create_dir_all(d);
        }
        if let Err(e) = std::fs::write(&p, text) {
            println!("cargo:warning=cannot write {}: {e}", p.display());
        }
    }
    fn prune(dir: &Path, root: &Path, keep: &BTreeMap<PathBuf, String>) {
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                prune(&p, root, keep);
                let _ = std::fs::remove_dir(&p);
            } else if p.extension().is_some_and(|x| x == "sl") && !keep.contains_key(p.strip_prefix(root).unwrap()) {
                let _ = std::fs::remove_file(&p);
            }
        }
    }
    prune(&out, &out, &files);
}
