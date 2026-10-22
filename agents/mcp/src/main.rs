//! `zell-mcp [--root dir]`: the Zell MCP server on stdio.

use std::path::PathBuf;

const USAGE: &str = "\
usage: zell-mcp [--root <dir>]

Model Context Protocol server for Zell on stdin/stdout (for AI coding agents).
The project root is --root, else $ZELL_MCP_ROOT, else the nearest directory
with a zell manifest above the current one, else the current directory.";

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut root: Option<PathBuf> = std::env::var_os("ZELL_MCP_ROOT").filter(|v| !v.is_empty()).map(PathBuf::from);
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--root" => {
                i += 1;
                let Some(v) = args.get(i) else {
                    eprintln!("error: --root needs a directory\n{USAGE}");
                    std::process::exit(2);
                };
                root = Some(PathBuf::from(v));
            }
            "-h" | "--help" => {
                println!("{USAGE}");
                return;
            }
            "-V" | "--version" => {
                println!("zell-mcp {}", env!("CARGO_PKG_VERSION"));
                return;
            }
            a => {
                eprintln!("error: unexpected argument `{a}`\n{USAGE}");
                std::process::exit(2);
            }
        }
        i += 1;
    }
    let root = root.unwrap_or_else(zell_mcp::Workspace::default_root);
    if let Err(e) = zell_mcp::run_stdio(root) {
        eprintln!("zell-mcp: {e}");
        std::process::exit(1);
    }
}
