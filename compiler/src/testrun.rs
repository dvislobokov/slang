//! `slang test`: finds `@test` functions, generates a driver and runs it.
//!
//! Discovery parses every `.sl` file of the path (the real parser, no
//! type checking) and takes each top-level `@test` function:
//! `func TestX(t *testing.T)` (a test), `func BenchX(b *testing.B)` (a
//! benchmark) or `func X()` (a test without a handle), and each `@bench
//! func BenchX(b *testing.B)`. A test whose body has the statement
//! `t.Parallel()` (on its `*testing.T` parameter) is generated with
//! `Parallel: true` and runs concurrently with the other parallel tests.
//! Per namespace with
//! tests a file is generated that lists them as `std.testing.Case`s
//! (in the same namespace, so unexported test functions work too), plus a
//! driver namespace whose `main` awaits `std.testing.Main` (stdlib_src/
//! testing.sl), which runs each test in a child process and prints the
//! summary. Existing `main` functions are renamed in memory so the driver
//! is the entry point. The sources keep their paths, so diagnostics and
//! `@embed` work as in a normal build.

use slang::ast;
use slang::diagnostics::Diagnostics;
use slang::span::SourceFile;
use std::collections::BTreeMap;
use std::path::PathBuf;

/// What a `@test` function takes.
#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Plain,
    Test,
    Bench,
}

const DRIVER_NS: &str = "slang_test_driver__";

/// Runs `slang test <args>`; returns the exit code.
pub fn run(args: &[String]) -> i32 {
    let mut path: Option<String> = None;
    let mut pass: Vec<String> = Vec::new();
    let mut build_args: Vec<String> = Vec::new();
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        match a.as_str() {
            "--json" | "--bench" | "-v" | "--verbose" => pass.push(a.clone()),
            "--filter" | "--run" | "--benchtime" | "--parallel" => {
                let Some(v) = args.get(i + 1) else {
                    eprintln!("error: {a} needs a value");
                    return 2;
                };
                pass.push(if a == "--run" { "--filter".into() } else { a.clone() });
                pass.push(v.clone());
                i += 1;
            }
            _ if a.starts_with('-') => build_args.push(a.clone()),
            _ if path.is_none() => path = Some(a.clone()),
            _ => build_args.push(a.clone()),
        }
        i += 1;
    }
    let root = PathBuf::from(path.unwrap_or_else(|| ".".into()));
    let mut opts = match slang::build::parse_args(&build_args) {
        Ok((o, _)) => o,
        Err(e) => {
            eprintln!("error: {e}");
            return 2;
        }
    };
    opts.input = Some(root.clone());
    let src_root = if root.join("src").is_dir() && (root.join("slang.toml").is_file() || root.join("slang.json").is_file() || root.join("slang.yaml").is_file()) {
        root.join("src")
    } else {
        root.clone()
    };
    let paths = match slang::driver::collect_files(&src_root) {
        Ok(p) if !p.is_empty() => p,
        Ok(_) => {
            eprintln!("error: no .sl files in {}", src_root.display());
            return 1;
        }
        Err(e) => {
            eprintln!("error: cannot read {}: {e}", src_root.display());
            return 1;
        }
    };
    let mut files = match slang::driver::load(&paths) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("error: cannot read source: {e}");
            return 1;
        }
    };
    let mut tests: BTreeMap<String, Vec<(String, Kind, bool)>> = BTreeMap::new();
    for f in &mut files {
        let mut diags = Diagnostics::new();
        let tokens = slang::lexer::lex(&f.text, &mut diags);
        let Some(file) = slang::parser::parse(tokens, &mut diags) else { continue };
        if diags.has_errors() {
            continue;
        }
        let ns = file.namespace.to_string();
        let mut mains: Vec<(usize, usize)> = Vec::new();
        for d in &file.decls {
            let ast::Decl::Func(func) = d else { continue };
            if func.name.name == "main" {
                mains.push((func.name.span.start as usize, func.name.span.end as usize));
                continue;
            }
            let is_test = func.attrs.iter().any(|a| a.path.to_string() == "test");
            let is_bench = func.attrs.iter().any(|a| a.path.to_string() == "bench");
            if !(is_test || is_bench) || !func.generics.is_empty() {
                continue;
            }
            let kind = match func.params.as_slice() {
                [] if !is_bench => Kind::Plain,
                [p] => match handle_type(&p.ty) {
                    Some("T") if !is_bench => Kind::Test,
                    Some("B") => Kind::Bench,
                    _ => continue,
                },
                _ => continue,
            };
            let parallel = kind == Kind::Test && calls_parallel(func);
            tests.entry(ns.clone()).or_default().push((func.name.name.clone(), kind, parallel));
        }
        // the driver's main is the entry point
        if !mains.is_empty() {
            let mut text = f.text.clone();
            for (s, e) in mains.into_iter().rev() {
                text.replace_range(s..e, "slangUserMain__");
            }
            *f = SourceFile::new(f.path.clone(), text);
        }
    }
    let total: usize = tests.values().map(|v| v.len()).sum();
    if total == 0 {
        println!("no tests found in {}", src_root.display());
        return 0;
    }
    let tmp = std::env::temp_dir().join(format!("slang-test-{}", std::process::id()));
    if let Err(e) = std::fs::create_dir_all(&tmp) {
        eprintln!("error: cannot create {}: {e}", tmp.display());
        return 1;
    }
    let mut driver = format!("namespace {DRIVER_NS}\n\nasync func main() {{\n    var cases []std.testing.Case = []\n");
    for (k, (ns, list)) in tests.iter().enumerate() {
        let mut gen = format!("namespace {ns}\n\n// generated by `slang test`\nfunc SlangTestCases{k}__() []std.testing.Case {{\n    var out []std.testing.Case = []\n");
        for (name, kind, parallel) in list {
            let case = match kind {
                Kind::Test if *parallel => format!("std.testing.Case{{Name: \"{name}\", Run: {name}, Parallel: true}}"),
                Kind::Test => format!("std.testing.Case{{Name: \"{name}\", Run: {name}}}"),
                Kind::Plain => format!("std.testing.Case{{Name: \"{name}\", Run: func(t *std.testing.T) {{\n        {name}()\n    }}}}"),
                Kind::Bench => format!("std.testing.Case{{Name: \"{name}\", Bench: {name}}}"),
            };
            gen.push_str(&format!("    out = append(out, {case})\n"));
        }
        gen.push_str("    return out\n}\n");
        files.push(SourceFile::new(tmp.join(format!("tests_{k}.sl")).display().to_string(), gen));
        driver.push_str(&format!("    for c in {ns}.SlangTestCases{k}__() {{\n        cases = append(cases, c)\n    }}\n"));
    }
    driver.push_str("    std.os.Exit(await std.testing.Main(cases))\n}\n");
    files.push(SourceFile::new(tmp.join("driver.sl").display().to_string(), driver));
    let exe = tmp.join("tests");
    let code = match slang::build::build_sources(files, &exe, &opts) {
        Ok(exe) => match std::process::Command::new(&exe).args(&pass).status() {
            Ok(st) => st.code().unwrap_or(1),
            Err(e) => {
                eprintln!("error: cannot run {}: {e}", exe.display());
                1
            }
        },
        Err(e) => {
            eprintln!("error: {e}");
            1
        }
    };
    let _ = std::fs::remove_dir_all(&tmp);
    code
}

/// True if the body of test `func` has the statement `t.Parallel()` on
/// its handle parameter (also nested in blocks, not in lambdas).
fn calls_parallel(func: &ast::FuncDecl) -> bool {
    fn in_block(b: &ast::Block, t: &str) -> bool {
        b.stmts.iter().any(|s| match &s.kind {
            ast::StmtKind::Expr(e) => match &e.kind {
                ast::ExprKind::Call { callee, args } if args.is_empty() => {
                    matches!(&callee.kind, ast::ExprKind::Field { base, name } if name.name == "Parallel" && matches!(&base.kind, ast::ExprKind::Ident(i) if i.name == t))
                }
                _ => false,
            },
            ast::StmtKind::Block(b) => in_block(b, t),
            ast::StmtKind::If(i) => in_block(&i.then, t),
            _ => false,
        })
    }
    let (Some(p), Some(body)) = (func.params.first(), &func.body) else { return false };
    in_block(body, &p.name.name)
}

/// `T` / `B` for a parameter typed `*testing.T` / `*std.testing.B` (any
/// path ending in T or B under a `testing` segment, or bare `*T`).
fn handle_type(t: &ast::TypeExpr) -> Option<&'static str> {
    let ast::TypeKind::Pointer(inner) = &t.kind else { return None };
    let ast::TypeKind::Named { path, generics } = &inner.kind else { return None };
    if !generics.is_empty() {
        return None;
    }
    let segs: Vec<&str> = path.segments.iter().map(|s| s.name.as_str()).collect();
    let last = *segs.last()?;
    let ok = segs.len() == 1 || segs[segs.len() - 2] == "testing";
    match (last, ok) {
        ("T", true) => Some("T"),
        ("B", true) => Some("B"),
        _ => None,
    }
}
