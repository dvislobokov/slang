//! `zell-lsp`: the Zell language server over stdio.

fn main() {
    if std::env::args().any(|a| a == "--version" || a == "-V") {
        println!("zell-lsp {}", env!("CARGO_PKG_VERSION"));
        return;
    }
    if let Err(e) = zell_lsp::run_stdio() {
        eprintln!("zell-lsp: {e}");
        std::process::exit(1);
    }
}
