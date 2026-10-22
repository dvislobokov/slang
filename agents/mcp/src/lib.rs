//! `zell-mcp`: a Model Context Protocol server that gives AI coding
//! agents (Claude Code, Codex CLI, Gemini CLI, Cursor, ...) Zell language
//! intelligence over stdio.
//!
//! The server is built on the compiler library's [`zell::session::Session`]
//! API, the same one `zell-lsp` uses, so its answers match the editor's:
//! diagnostics, definition, references, hover, outline, workspace and std
//! symbol search, rename and formatting run in-process; tests, builds and
//! runs shell out to the `zell` CLI.
//!
//! # Protocol
//!
//! The MCP subset is implemented by hand on `serde_json` (no SDK): the
//! stdio transport (one JSON-RPC 2.0 message per line, UTF-8, nothing else
//! on stdout), `initialize` with version negotiation ([`PROTOCOL_VERSIONS`]),
//! `notifications/initialized`, `ping`, `tools/list` and `tools/call`.
//! Tool results are text written for LLMs: `path:line:col` locations
//! (1-based, columns in characters, paths relative to the project root),
//! source snippets and unified diffs. A failed tool returns
//! `isError: true` with the reason; protocol errors are JSON-RPC errors.

mod shell;
pub mod tools;
mod workspace;

pub use workspace::Workspace;

use serde_json::{json, Value};
use std::io::{BufRead, Write};
use std::path::PathBuf;

/// MCP protocol revisions the server speaks, newest first. `initialize`
/// answers with the client's version when it is listed, else the newest.
pub const PROTOCOL_VERSIONS: &[&str] = &["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"];

/// Usage notes sent as `instructions` in the `initialize` result.
pub const INSTRUCTIONS: &str = "\
Zell language tools (zell-mcp). Positions are 1-based `line`/`col` (columns in characters); \
instead of a column you can pass `name` (the identifier on that line), or pass `symbol` \
(`User`, `User.Hello`, `app.models.User`, `strings.Split`) instead of a position. \
Run `diagnostics` after editing .zl files (it reads the files from disk); `rename` is a dry run \
unless `apply` is true; `format` rewrites the file unless `apply` is false. \
`std_index` and `doc` answer questions about the standard library offline; prefer them to guessing APIs.";

const PARSE_ERROR: i64 = -32700;
const INVALID_REQUEST: i64 = -32600;
const METHOD_NOT_FOUND: i64 = -32601;
const INVALID_PARAMS: i64 = -32602;

/// One MCP session over a project.
pub struct Server {
    ws: Workspace,
}

impl Server {
    /// A server for the project at `root` (files are loaded lazily, on
    /// the first tool call).
    pub fn new(root: PathBuf) -> Server {
        Server { ws: Workspace::new(root) }
    }

    /// Handles one JSON-RPC message; returns the response, or `None` for
    /// notifications and responses.
    pub fn handle(&mut self, msg: &Value) -> Option<Value> {
        let Some(method) = msg.get("method").and_then(Value::as_str) else {
            // A response to a server request (none are sent) or garbage.
            return msg.get("result").or_else(|| msg.get("error")).is_none().then(|| error(msg.get("id").cloned().unwrap_or(Value::Null), INVALID_REQUEST, "not a JSON-RPC request"));
        };
        let id = msg.get("id").cloned()?; // notifications: initialized, cancelled, ...
        let params = msg.get("params").cloned().unwrap_or(Value::Null);
        Some(match method {
            "initialize" => {
                let asked = params.get("protocolVersion").and_then(Value::as_str).unwrap_or("");
                let version = PROTOCOL_VERSIONS.iter().find(|v| **v == asked).copied().unwrap_or(PROTOCOL_VERSIONS[0]);
                ok(
                    id,
                    json!({
                        "protocolVersion": version,
                        "capabilities": { "tools": { "listChanged": false } },
                        "serverInfo": { "name": "zell-mcp", "title": "Zell", "version": env!("CARGO_PKG_VERSION") },
                        "instructions": INSTRUCTIONS,
                    }),
                )
            }
            "ping" => ok(id, json!({})),
            "tools/list" => ok(id, json!({ "tools": tools::list() })),
            "tools/call" => {
                let Some(name) = params.get("name").and_then(Value::as_str) else {
                    return Some(error(id, INVALID_PARAMS, "tools/call needs a tool name"));
                };
                let args = params.get("arguments").cloned().unwrap_or_else(|| json!({}));
                match tools::call(&mut self.ws, name, &args) {
                    None => error(id, INVALID_PARAMS, &format!("unknown tool `{name}`")),
                    Some(Ok(text)) => ok(id, json!({ "content": [{ "type": "text", "text": text }], "isError": false })),
                    Some(Err(text)) => ok(id, json!({ "content": [{ "type": "text", "text": text }], "isError": true })),
                }
            }
            _ => error(id, METHOD_NOT_FOUND, &format!("method `{method}` is not supported")),
        })
    }
}

fn ok(id: Value, result: Value) -> Value {
    json!({ "jsonrpc": "2.0", "id": id, "result": result })
}

fn error(id: Value, code: i64, message: &str) -> Value {
    json!({ "jsonrpc": "2.0", "id": id, "error": { "code": code, "message": message } })
}

/// Serves `input` (one message per line) to `output` until end of input.
pub fn run(server: &mut Server, input: impl BufRead, mut output: impl Write) -> std::io::Result<()> {
    for line in input.lines() {
        let line = line?;
        if line.trim().is_empty() {
            continue;
        }
        let reply = match serde_json::from_str::<Value>(&line) {
            Err(e) => Some(error(Value::Null, PARSE_ERROR, &format!("parse error: {e}"))),
            Ok(Value::Array(batch)) => {
                let out: Vec<Value> = batch.iter().filter_map(|m| server.handle(m)).collect();
                (!out.is_empty()).then_some(Value::Array(out))
            }
            Ok(msg) => server.handle(&msg),
        };
        if let Some(r) = reply {
            serde_json::to_writer(&mut output, &r)?;
            output.write_all(b"\n")?;
            output.flush()?;
        }
    }
    Ok(())
}

/// Runs the server on stdin/stdout for the project at `root`.
pub fn run_stdio(root: PathBuf) -> std::io::Result<()> {
    let mut server = Server::new(root);
    run(&mut server, std::io::stdin().lock(), std::io::stdout().lock())
}
