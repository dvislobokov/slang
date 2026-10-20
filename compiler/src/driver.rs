//! Front-end driver: loading source files and running lexer, parser and
//! semantic analysis over a whole project.

use crate::diagnostics::Diagnostics;
use crate::hir::Program;
use crate::span::SourceFile;
use crate::{ast, lexer, parser, sema};
use std::path::{Path, PathBuf};

/// The `.sl` files of `path`: the file itself, or every `.sl` file under a
/// directory (recursively), sorted by path for deterministic output.
pub fn collect_files(path: &Path) -> std::io::Result<Vec<PathBuf>> {
    if path.is_file() {
        return Ok(vec![path.to_path_buf()]);
    }
    let mut out = Vec::new();
    let mut stack = vec![path.to_path_buf()];
    while let Some(dir) = stack.pop() {
        for entry in std::fs::read_dir(&dir)? {
            let p = entry?.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.extension().is_some_and(|e| e == "sl") {
                out.push(p);
            }
        }
    }
    out.sort();
    Ok(out)
}

/// Reads `paths` into source files.
pub fn load(paths: &[PathBuf]) -> std::io::Result<Vec<SourceFile>> {
    paths.iter().map(|p| Ok(SourceFile::new(p.display().to_string(), std::fs::read_to_string(p)?))).collect()
}

/// Lexes and parses every file; diagnostics get their file index.
/// Returns `None` if any file failed to parse.
pub fn parse_all(files: &[SourceFile], diags: &mut Diagnostics) -> Option<Vec<ast::File>> {
    let mut asts = Vec::new();
    let mut ok = true;
    for (i, f) in files.iter().enumerate() {
        let start = diags.items.len();
        let tokens = lexer::lex(&f.text, diags);
        let parsed = if diags.items[start..].iter().any(|d| d.severity == crate::diagnostics::Severity::Error) {
            None
        } else {
            parser::parse(tokens, diags)
        };
        for d in &mut diags.items[start..] {
            d.file = i;
        }
        match parsed {
            Some(a) => asts.push(a),
            None => ok = false,
        }
    }
    ok.then_some(asts)
}

/// Parses and analyses a project.
pub fn check(files: &[SourceFile], diags: &mut Diagnostics) -> Option<Program> {
    let asts = parse_all(files, diags)?;
    sema::analyze(&asts, diags)
}
