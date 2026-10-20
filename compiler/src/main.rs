use std::io::Write;
use std::process::exit;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: slang <parse|run|build> <file.sl>");
        exit(2);
    }
    let path = &args[2];
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("error: cannot read {path}: {e}");
            exit(1);
        }
    };
    let file = slang::span::SourceFile::new(path.clone(), text);
    let mut diags = slang::diagnostics::Diagnostics::new();
    match args[1].as_str() {
        "parse" => {
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
        cmd => {
            eprintln!("error: command `{cmd}` is not implemented yet");
            exit(2);
        }
    }
}
