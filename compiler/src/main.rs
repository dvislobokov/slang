use std::io::Write;
use std::path::PathBuf;
use std::process::exit;

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
  test [path] [options]          type-check with dev-dependencies (no test runner yet)
  manifest schema                print the manifest JSON Schema
  manifest convert <toml|json|yaml> [dir]
                                 rewrite the manifest in another format
  parse <file.sl>                print the AST
  lsp                            run the language server on stdio (execs slang-lsp)

build options:
  -o <path>            output file (single package)
  --release            -O2, no -rdynamic, --gc-sections, stripped
  -O0..-O3             optimisation level
  --static             fully static executable
  --leak-check         report objects still alive at exit, exit non-zero
  --emit=exe|obj|llvm-ir
  --workspace          all workspace members
  -p, --package <name> one workspace member (repeatable)
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
        "check" | "test" => {
            let dump = rest.iter().any(|a| a == "--dump-hir");
            let flags: Vec<String> = rest.iter().filter(|a| *a != "--dump-hir").cloned().collect();
            let opts = parse_build_args(&flags).0;
            let test = args[1] == "test";
            match slang::build::check(&opts, dump, test) {
                Ok(n) if test => eprintln!("slang test: {n} package(s) checked; running @test functions is not implemented yet"),
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
        "init" => init(rest),
        "manifest" => manifest(rest),
        "lsp" => lsp(rest),
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
