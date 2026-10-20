use std::io::Write;
use std::path::Path;
use std::process::exit;

use slang::diagnostics::Diagnostics;
use slang::driver;

const USAGE: &str = "usage: slang parse <file.sl>\n       slang check [--dump-hir] <file.sl | dir>";

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("{USAGE}");
        exit(2);
    }
    match args[1].as_str() {
        "parse" => parse(&args[2]),
        "check" => {
            let dump = args[2..].iter().any(|a| a == "--dump-hir");
            let Some(path) = args[2..].iter().find(|a| !a.starts_with("--")) else {
                eprintln!("{USAGE}");
                exit(2);
            };
            check(path, dump);
        }
        cmd => {
            eprintln!("error: command `{cmd}` is not implemented yet");
            exit(2);
        }
    }
}

fn read(path: &str) -> slang::span::SourceFile {
    match std::fs::read_to_string(path) {
        Ok(t) => slang::span::SourceFile::new(path.to_string(), t),
        Err(e) => {
            eprintln!("error: cannot read {path}: {e}");
            exit(1);
        }
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

fn check(path: &str, dump: bool) {
    let paths = match driver::collect_files(Path::new(path)) {
        Ok(p) if !p.is_empty() => p,
        Ok(_) => {
            eprintln!("error: no .sl files in {path}");
            exit(1);
        }
        Err(e) => {
            eprintln!("error: cannot read {path}: {e}");
            exit(1);
        }
    };
    let files = match driver::load(&paths) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("error: cannot read source: {e}");
            exit(1);
        }
    };
    let mut diags = Diagnostics::new();
    let program = driver::check(&files, &mut diags);
    if !diags.is_empty() {
        eprint!("{}", diags.render_all_files(&files));
    }
    match program {
        Some(p) if !diags.has_errors() => {
            if dump {
                let _ = write!(std::io::stdout(), "{}", p.dump());
            }
        }
        _ => exit(1),
    }
}
