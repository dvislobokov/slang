use std::io::Write;
use std::path::PathBuf;
use std::process::exit;

mod testrun;

use slang::diagnostics::Diagnostics;
use slang::package::{self, Format};

const USAGE: &str = "\
usage: slang <command> [args]

commands:
  init [dir] [--name n] [--format toml|json|yaml] [--lib]
                                 create a package (manifest + src/main.sl or src/lib.sl)
  build [path] [options]         build the project found from the current directory,
                                 a project directory, a plain directory or a single file
  run [path] [options] [-- args] build and run
  check [path] [--dump-hir] [options]
                                 type-check without generating code
  test [path] [--filter re] [--json] [--bench] [-v] [--parallel n] [options]
                                 run the @test functions (and @test benchmarks
                                 taking *testing.B with --bench), see std.testing
  debug [--lldb|--gdb] [path] [options] [-- args]
                                 build with debug info and start lldb (default) or gdb
                                 with the Slang formatters loaded (docs/DEBUGGER.md)
                                 (--source <file>: run debugger commands, --batch: then exit)
  debug --formatters lldb|gdb    print the path of the formatter script
  manifest schema                print the manifest JSON Schema
  manifest convert <toml|json|yaml> [dir]
                                 rewrite the manifest in another format
  parse <file.sl>                print the AST
  fmt [paths] [--check] [--diff] [--stdin]
                                 format .sl files in place (canonical style)
  lsp                            run the language server on stdio (execs slang-lsp)
  tool pprof [args] <profile>    inspect a std.diag profile (runs `go tool pprof`)

build options:
  -o <path>            output file (single package)
  --release            -O2, no -rdynamic, --gc-sections, stripped
  -g                   keep DWARF debug info in a --release build (debug builds have it)
  --strip              no debug info, stripped executable
  -O0..-O3             optimisation level
  --static             fully static executable
  --leak-check         report objects still alive at exit, exit non-zero
  --no-error-trace     do not record error return traces (errors.Trace is empty)
  --emit=exe|obj|llvm-ir
  --workspace          all workspace members
  -p, --package <name> one workspace member (repeatable)
  --target <t>         target for @cfg, file suffixes and os.Name/os.Arch
                       (triple or alias such as linux-amd64; non-host only for check)
  --locked             fail if slang.lock would change
  -v, --verbose        log the object cache and the linker";

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 || matches!(args[1].as_str(), "-h" | "--help" | "help") {
        eprintln!("{USAGE}");
        exit(if args.len() < 2 { 2 } else { 0 });
    }
    let rest = &args[2..];
    match args[1].as_str() {
        "parse" => match rest.first() {
            Some(p) => parse(p),
            None => usage_error("parse needs a file"),
        },
        "test" => exit(testrun::run(rest)),
        "check" => {
            let dump = rest.iter().any(|a| a == "--dump-hir");
            let flags: Vec<String> = rest.iter().filter(|a| *a != "--dump-hir").cloned().collect();
            let opts = parse_build_args(&flags).0;
            match slang::build::check(&opts, dump, false) {
                Ok(_) => {}
                Err(e) => fail(&e),
            }
        }
        "build" => {
            let opts = parse_build_args(rest).0;
            if let Err(e) = slang::build::build(&opts) {
                fail(&e);
            }
        }
        "run" => {
            let (opts, extra) = parse_build_args(rest);
            match slang::build::run(opts, &extra) {
                Ok(code) => exit(code),
                Err(e) => fail(&e),
            }
        }
        "debug" => debug(rest),
        "init" => init(rest),
        "manifest" => manifest(rest),
        "lsp" => lsp(rest),
        "tool" => tool(rest),
        "fmt" => exit(slang::fmt::cli::run(rest)),
        cmd => {
            eprintln!("error: command `{cmd}` is not implemented yet\n{USAGE}");
            exit(2);
        }
    }
}

fn fail(e: &str) -> ! {
    eprintln!("error: {e}");
    exit(1);
}

fn usage_error(e: &str) -> ! {
    eprintln!("error: {e}\n\n{USAGE}");
    exit(2);
}

fn parse_build_args(args: &[String]) -> (slang::build::BuildOptions, Vec<String>) {
    slang::build::parse_args(args).unwrap_or_else(|e| usage_error(&e))
}

fn init(args: &[String]) {
    let mut dir: Option<PathBuf> = None;
    let mut name = None;
    let mut format = Format::Toml;
    let mut lib = false;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--lib" => lib = true,
            "--name" => {
                i += 1;
                name = Some(args.get(i).cloned().unwrap_or_else(|| usage_error("--name needs a value")));
            }
            "--format" => {
                i += 1;
                let f = args.get(i).unwrap_or_else(|| usage_error("--format needs toml, json or yaml"));
                format = Format::parse(f).unwrap_or_else(|| usage_error(&format!("unknown manifest format `{f}` (toml, json, yaml)")));
            }
            a if a.starts_with("--format=") => {
                let f = &a["--format=".len()..];
                format = Format::parse(f).unwrap_or_else(|| usage_error(&format!("unknown manifest format `{f}` (toml, json, yaml)")));
            }
            a if a.starts_with('-') => usage_error(&format!("unknown option `{a}`")),
            a => {
                if dir.is_some() {
                    usage_error(&format!("unexpected argument `{a}`"));
                }
                dir = Some(PathBuf::from(a));
            }
        }
        i += 1;
    }
    // `slang init name` creates ./name; the package is named after the directory.
    let dir = dir.unwrap_or_else(|| PathBuf::from("."));
    match package::init::init(&dir, name.as_deref(), format, lib) {
        Ok(files) => {
            for f in files {
                eprintln!("created {}", f.display());
            }
        }
        Err(e) => fail(&e),
    }
}

fn manifest(args: &[String]) {
    match args.first().map(String::as_str) {
        Some("schema") => {
            let _ = std::io::stdout().write_all(package::Manifest::json_schema().as_bytes());
        }
        Some("convert") => {
            let Some(f) = args.get(1) else { usage_error("manifest convert needs a format (toml, json, yaml)") };
            let format = Format::parse(f).unwrap_or_else(|| usage_error(&format!("unknown manifest format `{f}` (toml, json, yaml)")));
            let dir = match args.get(2) {
                Some(d) => PathBuf::from(d),
                None => {
                    let cwd = std::env::current_dir().unwrap_or_else(|e| fail(&e.to_string()));
                    match package::manifest::find_upwards(&cwd) {
                        Ok(Some(p)) => p.parent().unwrap().to_path_buf(),
                        Ok(None) => fail("no manifest in the current directory or any parent"),
                        Err(e) => fail(&e),
                    }
                }
            };
            match package::manifest::convert(&dir, format) {
                Ok((old, new)) if old == new => eprintln!("{} is already {format}", new.display()),
                Ok((old, new)) => eprintln!("converted {} -> {}", old.display(), new.display()),
                Err(e) => fail(&e),
            }
        }
        _ => usage_error("usage: slang manifest schema | slang manifest convert <toml|json|yaml> [dir]"),
    }
}

fn read(path: &str) -> slang::span::SourceFile {
    match std::fs::read_to_string(path) {
        Ok(t) => slang::span::SourceFile::new(path.to_string(), t),
        Err(e) => fail(&format!("cannot read {path}: {e}")),
    }
}

fn parse(path: &str) {
    let file = read(path);
    let mut diags = Diagnostics::new();
    let tokens = slang::lexer::lex(&file.text, &mut diags);
    let ast = if diags.has_errors() { None } else { slang::parser::parse(tokens, &mut diags) };
    if !diags.is_empty() {
        eprint!("{}", diags.render_all(&file));
    }
    if diags.has_errors() {
        exit(1);
    }
    // Ignore write errors such as a closed pipe (`slang parse f | head`).
    let _ = writeln!(std::io::stdout(), "{:#?}", ast.unwrap());
}

/// `slang tool <name> [args]`: diagnostics tools. `pprof` shells out to
/// `go tool pprof` (or a `pprof` binary on PATH), which reads the
/// `profile.proto` files written by `diag.StopCPUProfile` and
/// `diag.WriteHeapProfile`.
fn tool(args: &[String]) {
    match args.first().map(String::as_str) {
        Some("pprof") => {
            let attempts: [(&str, &[&str]); 2] = [("go", &["tool", "pprof"]), ("pprof", &[])];
            for (program, pre) in attempts {
                match std::process::Command::new(program).args(pre).args(&args[1..]).status() {
                    Ok(status) => exit(status.code().unwrap_or(1)),
                    Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
                    Err(e) => fail(&format!("cannot start {program}: {e}")),
                }
            }
            fail("slang tool pprof needs `go` (go tool pprof) or `pprof` on PATH")
        }
        Some(t) => usage_error(&format!("unknown tool `{t}` (available: pprof)")),
        None => usage_error("tool needs a name (available: pprof)"),
    }
}

/// `slang lsp`: runs the `slang-lsp` binary (next to this executable, else
/// on PATH) with stdio inherited, so editors can launch either command.
fn lsp(args: &[String]) {
    let exe = if cfg!(windows) { "slang-lsp.exe" } else { "slang-lsp" };
    let sibling = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.join(exe))).filter(|p| p.is_file());
    let program = sibling.unwrap_or_else(|| PathBuf::from(exe));
    match std::process::Command::new(&program).args(args).status() {
        Ok(status) => exit(status.code().unwrap_or(1)),
        Err(e) => fail(&format!("cannot start {}: {e} (build it with `cargo build -p slang-lsp`)", program.display())),
    }
}

/// The lldb formatters and gdb printers (tools/), embedded so that an
/// installed compiler works without the repository.
const LLDB_FORMATTERS: &str = include_str!("../../tools/lldb/slang_formatters.py");
const GDB_PRINTERS: &str = include_str!("../../tools/gdb/slang_printers.py");

/// Path of the formatter script for `which` (`lldb` or `gdb`): `$SLANG_TOOLS`,
/// `tools/` of the source tree next to the compiler, or the embedded copy
/// written to the temporary directory.
fn formatter_script(which: &str) -> PathBuf {
    let (rel, embedded) = match which {
        "gdb" => ("gdb/slang_printers.py", GDB_PRINTERS),
        _ => ("lldb/slang_formatters.py", LLDB_FORMATTERS),
    };
    let mut roots = Vec::new();
    if let Ok(t) = std::env::var("SLANG_TOOLS") {
        roots.push(PathBuf::from(t));
    }
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.to_path_buf())) {
        roots.push(dir.join("../../tools"));
        roots.push(dir.join("../../../tools"));
        roots.push(dir.join("../share/slang/tools"));
    }
    roots.push(PathBuf::from(concat!(env!("CARGO_MANIFEST_DIR"), "/../tools")));
    for r in roots {
        let p = r.join(rel);
        if p.is_file() {
            return p.canonicalize().unwrap_or(p);
        }
    }
    let dir = std::env::temp_dir().join(format!("slang-tools-{}", env!("CARGO_PKG_VERSION")));
    let p = dir.join(rel);
    if let Some(parent) = p.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if std::fs::read_to_string(&p).ok().as_deref() != Some(embedded) {
        if let Err(e) = std::fs::write(&p, embedded) {
            fail(&format!("cannot write {}: {e}", p.display()));
        }
    }
    p
}

/// `slang debug [--lldb|--gdb] [path] [build options] [-- args]`: builds
/// with debug info and runs the debugger on the executable with the Slang
/// formatters loaded. `slang debug --formatters lldb|gdb` prints the script
/// path (used by the VS Code extension for `initCommands`).
fn debug(args: &[String]) {
    let mut gdb = false;
    let mut batch = false;
    let mut source: Option<String> = None;
    let mut flags = Vec::new();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--" => {
                flags.extend_from_slice(&args[i..]);
                break;
            }
            "--lldb" => gdb = false,
            "--gdb" => gdb = true,
            "--batch" => batch = true,
            "--source" => {
                i += 1;
                source = Some(args.get(i).cloned().unwrap_or_else(|| usage_error("--source needs a command file")));
            }
            "--formatters" => {
                let which = args.get(i + 1).map(String::as_str).unwrap_or("lldb");
                if which != "lldb" && which != "gdb" {
                    usage_error("--formatters takes lldb or gdb");
                }
                println!("{}", formatter_script(which).display());
                return;
            }
            a => flags.push(a.to_string()),
        }
        i += 1;
    }
    let (mut opts, prog_args) = parse_build_args(&flags);
    opts.emit = slang::build::Emit::Exe;
    opts.strip = false;
    opts.debug_info = true;
    let tmp = std::env::temp_dir().join(format!("slang-debug-{}", std::process::id()));
    let plain = opts.input.as_ref().is_some_and(|p| p.is_file());
    if plain && opts.output.is_none() {
        if let Err(e) = std::fs::create_dir_all(&tmp) {
            fail(&format!("cannot create {}: {e}", tmp.display()));
        }
        let stem = opts.input.as_ref().and_then(|p| p.file_stem()).map(|s| s.to_string_lossy().to_string()).unwrap_or_else(|| "prog".into());
        opts.output = Some(tmp.join(stem));
    }
    let exes = slang::build::build(&opts).unwrap_or_else(|e| fail(&e));
    let exe = match exes.as_slice() {
        [e] => e.clone(),
        [] => fail("nothing to debug: no executable was built"),
        _ => fail("several executables were built; choose one with -p"),
    };
    let mut cmd = if gdb {
        let mut c = std::process::Command::new("gdb");
        c.arg("-q").arg("-ex").arg(format!("source {}", formatter_script("gdb").display()));
        if batch {
            c.arg("--batch");
        }
        if let Some(s) = &source {
            c.arg("-x").arg(s);
        }
        c.arg("--args").arg(&exe).args(&prog_args);
        c
    } else {
        let lldb = ["lldb", "lldb-18"].into_iter().find(|l| std::process::Command::new(l).arg("--version").output().is_ok_and(|o| o.status.success())).unwrap_or("lldb");
        let mut c = std::process::Command::new(lldb);
        c.arg("-O").arg(format!("command script import {}", formatter_script("lldb").display()));
        if batch {
            c.arg("--batch");
        }
        if let Some(s) = &source {
            c.arg("-s").arg(s);
        }
        c.arg("--").arg(&exe).args(&prog_args);
        c
    };
    let status = cmd.status();
    let _ = std::fs::remove_dir_all(&tmp);
    match status {
        Ok(s) => exit(s.code().unwrap_or(1)),
        Err(e) => fail(&format!("cannot start the debugger: {e}")),
    }
}
