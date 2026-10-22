//! `zell fmt [paths] [--check] [--diff] [--stdin]`.

use super::{format_source, unified_diff, FmtError};
use crate::span::SourceFile;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

pub const USAGE: &str = "usage: zell fmt [paths...] [--check] [--diff] [--stdin]
  paths    files or directories (recursively, .zl files); default: .
  --check  do not write; list files that are not formatted, exit 1 if any
  --diff   do not write; print a unified diff of the changes
  --stdin  format standard input to standard output";

/// Runs the command; returns the process exit code.
pub fn run(args: &[String]) -> i32 {
    let (mut check, mut diff, mut stdin) = (false, false, false);
    let mut paths = Vec::new();
    for a in args {
        match a.as_str() {
            "--check" => check = true,
            "--diff" => diff = true,
            "--stdin" => stdin = true,
            "-h" | "--help" => {
                eprintln!("{USAGE}");
                return 0;
            }
            s if s.starts_with('-') => {
                eprintln!("error: unknown option {s}\n{USAGE}");
                return 2;
            }
            s => paths.push(PathBuf::from(s)),
        }
    }
    if stdin {
        let mut src = String::new();
        if std::io::stdin().read_to_string(&mut src).is_err() {
            eprintln!("error: cannot read standard input");
            return 1;
        }
        return match format_source(&src) {
            Ok(out) => {
                let _ = std::io::stdout().write_all(out.as_bytes());
                0
            }
            Err(e) => {
                report(&SourceFile::new("<stdin>", src), &e);
                1
            }
        };
    }
    if paths.is_empty() {
        paths.push(PathBuf::from("."));
    }
    let mut files = Vec::new();
    for p in &paths {
        if p.is_file() {
            files.push(p.clone());
        } else if p.is_dir() {
            collect(p, &mut files);
        } else {
            eprintln!("error: {}: no such file or directory", p.display());
            return 1;
        }
    }
    files.sort();
    let (mut changed, mut failed) = (0usize, 0usize);
    for f in &files {
        let Ok(src) = std::fs::read_to_string(f) else {
            eprintln!("error: cannot read {}", f.display());
            failed += 1;
            continue;
        };
        let out = match format_source(&src) {
            Ok(o) => o,
            Err(e) => {
                report(&SourceFile::new(f.display().to_string(), src), &e);
                failed += 1;
                continue;
            }
        };
        if out == src {
            continue;
        }
        changed += 1;
        if diff {
            print!("{}", unified_diff(&f.display().to_string(), &src, &out));
        }
        if check {
            if !diff {
                println!("{}", f.display());
            }
        } else if !diff {
            if let Err(e) = std::fs::write(f, &out) {
                eprintln!("error: cannot write {}: {e}", f.display());
                failed += 1;
            }
        }
    }
    let verb = if check || diff { "would be reformatted" } else { "reformatted" };
    eprintln!("zell fmt: {changed} of {} file(s) {verb}{}", files.len(), if failed > 0 { format!(", {failed} failed") } else { String::new() });
    if failed > 0 || (check && changed > 0) {
        1
    } else {
        0
    }
}

fn report(file: &SourceFile, e: &FmtError) {
    match e {
        FmtError::Syntax(d) => eprint!("{}", d.render_all(file)),
        FmtError::Internal(m) => eprintln!("error: {}: internal formatter error: {m} (file left unchanged)", file.path),
    }
}

/// `.zl` files under `dir`, skipping hidden and build directories.
fn collect(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for e in entries.flatten() {
        let p = e.path();
        let name = e.file_name().to_string_lossy().to_string();
        if p.is_dir() {
            if !name.starts_with('.') && !matches!(name.as_str(), "target" | "node_modules" | "build" | "out") {
                collect(&p, out);
            }
        } else if p.extension().is_some_and(|x| x == "zl") {
            out.push(p);
        }
    }
}
