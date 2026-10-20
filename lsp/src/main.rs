//! `slang-lsp`: the Slang language server over stdio.

fn main() {
    if std::env::args().any(|a| a == "--version" || a == "-V") {
        println!("slang-lsp {}", env!("CARGO_PKG_VERSION"));
        return;
    }
    if let Err(e) = slang_lsp::run_stdio() {
        eprintln!("slang-lsp: {e}");
        std::process::exit(1);
    }
}
