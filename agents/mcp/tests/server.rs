//! Drives the `zell-mcp` binary over its stdio transport, like an agent
//! would: one JSON-RPC message per line in, one per line out.

use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::atomic::{AtomicUsize, Ordering};

struct Client {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    next_id: i64,
}

impl Client {
    fn start(root: &Path) -> Client {
        let mut child = Command::new(env!("CARGO_BIN_EXE_zell-mcp"))
            .arg("--root")
            .arg(root)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .expect("start zell-mcp");
        let stdin = child.stdin.take().unwrap();
        let stdout = BufReader::new(child.stdout.take().unwrap());
        let mut c = Client { child, stdin, stdout, next_id: 0 };
        let init = c.request("initialize", json!({ "protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": { "name": "test", "version": "1" } }));
        assert_eq!(init["result"]["protocolVersion"], "2025-06-18", "{init}");
        assert_eq!(init["result"]["serverInfo"]["name"], "zell-mcp");
        assert!(init["result"]["capabilities"]["tools"].is_object());
        c.send_raw(&json!({ "jsonrpc": "2.0", "method": "notifications/initialized" }).to_string());
        c
    }

    fn send_raw(&mut self, line: &str) {
        writeln!(self.stdin, "{line}").unwrap();
        self.stdin.flush().unwrap();
    }

    fn read(&mut self) -> Value {
        let mut line = String::new();
        self.stdout.read_line(&mut line).unwrap();
        serde_json::from_str(&line).unwrap_or_else(|e| panic!("bad line {line:?}: {e}"))
    }

    fn request(&mut self, method: &str, params: Value) -> Value {
        self.next_id += 1;
        let id = self.next_id;
        self.send_raw(&json!({ "jsonrpc": "2.0", "id": id, "method": method, "params": params }).to_string());
        let r = self.read();
        assert_eq!(r["id"], id, "{r}");
        assert_eq!(r["jsonrpc"], "2.0");
        r
    }

    /// Calls a tool; returns (text, isError).
    fn call(&mut self, tool: &str, args: Value) -> (String, bool) {
        let r = self.request("tools/call", json!({ "name": tool, "arguments": args }));
        let res = &r["result"];
        assert!(res.is_object(), "{r}");
        (res["content"][0]["text"].as_str().unwrap().to_string(), res["isError"].as_bool().unwrap())
    }

    fn ok(&mut self, tool: &str, args: Value) -> String {
        let (text, err) = self.call(tool, args);
        assert!(!err, "{tool} failed: {text}");
        text
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

const MODELS: &str = "namespace demo.models\n\n// A user of the system.\nstruct User {\n    Name string\n\n    // Hello greets the user.\n    func Hello() string {\n        return \"hi \" + this.Name\n    }\n}\n";
const MAIN: &str = "namespace demo\n\nimport io\nimport demo.models\n\nfunc main() {\n    var u = User{Name: \"zell\"}\n    println(u.Hello())\n}\n";
const TESTS: &str = "namespace demo.models\n\n@test func TestHello(t *testing.T) {\n    var u = User{Name: \"x\"}\n    testing.Equal(t, u.Hello(), \"hi x\")\n}\n\n@test func TestBroken(t *testing.T) {\n    testing.Equal(t, 1 + 1, 3)\n}\n";

/// A fresh project: zell.toml, src/main.zl, src/models.zl.
fn project() -> PathBuf {
    static N: AtomicUsize = AtomicUsize::new(0);
    let dir = std::env::temp_dir().join(format!("zell-mcp-test-{}-{}", std::process::id(), N.fetch_add(1, Ordering::Relaxed)));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(dir.join("src")).unwrap();
    std::fs::write(dir.join("zell.toml"), "[package]\nname = \"demo\"\nversion = \"0.1.0\"\nnamespace = \"demo\"\nentry = \"src/main.zl\"\n").unwrap();
    std::fs::write(dir.join("src/main.zl"), MAIN).unwrap();
    std::fs::write(dir.join("src/models.zl"), MODELS).unwrap();
    dir.canonicalize().unwrap()
}

#[test]
fn lists_tools_and_answers_protocol_errors() {
    let root = project();
    let mut c = Client::start(&root);
    let tools = c.request("tools/list", json!({}));
    let names: Vec<&str> = tools["result"]["tools"].as_array().unwrap().iter().map(|t| t["name"].as_str().unwrap()).collect();
    for want in ["diagnostics", "definition", "references", "hover", "document_symbols", "workspace_symbols", "rename", "format", "run_tests", "build", "run", "doc", "std_index"] {
        assert!(names.contains(&want), "{want} missing from {names:?}");
    }
    for t in tools["result"]["tools"].as_array().unwrap() {
        assert_eq!(t["inputSchema"]["type"], "object", "{t}");
        assert!(t["description"].as_str().is_some_and(|d| d.len() > 20));
    }
    assert_eq!(c.request("ping", json!({}))["result"], json!({}));
    assert_eq!(c.request("resources/list", json!({}))["error"]["code"], -32601);
    assert_eq!(c.request("tools/call", json!({ "name": "nope" }))["error"]["code"], -32602);
    c.send_raw("not json");
    assert_eq!(c.read()["error"]["code"], -32700);
    // A batch: the notification gets no reply.
    c.send_raw(&json!([{ "jsonrpc": "2.0", "method": "notifications/cancelled" }, { "jsonrpc": "2.0", "id": "b", "method": "ping" }]).to_string());
    let batch = c.read();
    assert_eq!(batch.as_array().map(Vec::len), Some(1), "{batch}");
    assert_eq!(batch[0]["id"], "b");
    // An unknown protocol version gets the newest.
    let v = c.request("initialize", json!({ "protocolVersion": "1999-01-01" }));
    assert_eq!(v["result"]["protocolVersion"], zell_mcp::PROTOCOL_VERSIONS[0]);
}

#[test]
fn diagnostics_follow_the_files_on_disk() {
    let root = project();
    let mut c = Client::start(&root);
    assert!(c.ok("diagnostics", json!({})).starts_with("No errors or warnings in 2 files"));
    std::fs::write(root.join("src/main.zl"), MAIN.replace("    println(u.Hello())\n", "    println(u.Hello())\n    var unused = 1\n")).unwrap();
    let text = c.ok("diagnostics", json!({}));
    assert!(text.contains("src/main.zl:9:9: error[E0001]: unused variable unused"), "{text}");
    assert!(text.contains("    | var unused = 1"), "{text}");
    assert!(text.contains("fix: Rename unused to _"), "{text}");
    let one = c.ok("diagnostics", json!({ "path": "src/models.zl" }));
    assert!(one.starts_with("No errors"), "{one}");
    let (missing, err) = c.call("diagnostics", json!({ "path": "src/nope.zl" }));
    assert!(err && missing.contains("no such file"), "{missing}");
    // A new file is picked up, too.
    std::fs::write(root.join("src/extra.zl"), "namespace demo.extra\n\nfunc F() int {\n    return \"s\"\n}\n").unwrap();
    let text = c.ok("diagnostics", json!({}));
    assert!(text.contains("src/extra.zl:4:"), "{text}");
}

#[test]
fn navigates_by_position_name_and_symbol() {
    let root = project();
    let mut c = Client::start(&root);
    // line 8: `    println(u.Hello())`
    let by_col = c.ok("definition", json!({ "path": "src/main.zl", "line": 8, "col": 16 }));
    assert!(by_col.starts_with("method demo.models.User.Hello\nsrc/models.zl:8:10\n"), "{by_col}");
    assert!(by_col.contains("    8 |     func Hello() string {"), "{by_col}");
    let by_name = c.ok("definition", json!({ "path": "src/main.zl", "line": 8, "name": "Hello" }));
    assert_eq!(by_name, by_col);
    let by_symbol = c.ok("definition", json!({ "symbol": "User.Hello" }));
    assert_eq!(by_symbol, by_col);
    let (none, err) = c.call("definition", json!({ "path": "src/main.zl", "line": 2, "col": 1 }));
    assert!(err && none.contains("no symbol at src/main.zl:2:1"), "{none}");

    let refs = c.ok("references", json!({ "symbol": "demo.models.User" }));
    assert!(refs.starts_with("struct demo.models.User: 1 reference in 2 files"), "{refs}");
    assert!(refs.contains("src/main.zl:7:13: var u = User{Name: \"zell\"}"), "{refs}");
    assert!(refs.contains("src/models.zl:4:8 (declaration): struct User {"), "{refs}");

    let hover = c.ok("hover", json!({ "path": "src/main.zl", "line": 7, "name": "u" }));
    assert!(hover.contains("var u User") && hover.contains("local variable in function demo.main, type User"), "{hover}");
    let doc = c.ok("hover", json!({ "symbol": "Hello" }));
    assert!(doc.contains("func Hello() string") && doc.contains("Hello greets the user."), "{doc}");

    let outline = c.ok("document_symbols", json!({ "path": "src/models.zl" }));
    assert!(outline.contains("    1  namespace demo.models"), "{outline}");
    assert!(outline.contains("    4    struct User"), "{outline}");
    assert!(outline.contains("    8      method Hello: func Hello() string"), "{outline}");

    let found = c.ok("workspace_symbols", json!({ "query": "usr" }));
    assert!(found.contains("struct demo.models.User  src/models.zl:4:8"), "{found}");
    let none = c.ok("workspace_symbols", json!({ "query": "zzzq" }));
    assert!(none.starts_with("No declarations match"), "{none}");
}

#[test]
fn renames_as_a_dry_run_then_applies() {
    let root = project();
    let mut c = Client::start(&root);
    let dry = c.ok("rename", json!({ "symbol": "User.Hello", "new_name": "Greet" }));
    assert!(dry.starts_with("Rename method demo.models.User.Hello to Greet: 2 edits in 2 files"), "{dry}");
    assert!(dry.contains("-    println(u.Hello())\n+    println(u.Greet())"), "{dry}");
    assert!(dry.contains("+++ src/models.zl\n"), "{dry}");
    assert!(dry.contains("Dry run"), "{dry}");
    assert_eq!(std::fs::read_to_string(root.join("src/main.zl")).unwrap(), MAIN, "a dry run writes nothing");
    let (bad, err) = c.call("rename", json!({ "symbol": "User.Hello", "new_name": "func" }));
    assert!(err && bad.contains("not a valid Zell identifier"), "{bad}");
    let (std, err) = c.call("rename", json!({ "path": "src/main.zl", "line": 8, "name": "println", "new_name": "say" }));
    assert!(err && std.contains("cannot be renamed"), "{std}");
    let applied = c.ok("rename", json!({ "path": "src/models.zl", "line": 8, "name": "Hello", "new_name": "Greet", "apply": true }));
    assert!(applied.contains("Applied."), "{applied}");
    assert!(std::fs::read_to_string(root.join("src/main.zl")).unwrap().contains("u.Greet()"));
    assert!(c.ok("diagnostics", json!({})).starts_with("No errors"));
}

#[test]
fn formats_files() {
    let root = project();
    let ugly = "namespace demo.models\n\nfunc   Twice(x int)int{\nreturn x*2\n}\n";
    std::fs::write(root.join("src/ugly.zl"), ugly).unwrap();
    let mut c = Client::start(&root);
    let check = c.ok("format", json!({ "path": "src/ugly.zl", "apply": false }));
    assert!(check.starts_with("1 file of 1 would change (not written)"), "{check}");
    assert!(check.contains("+func Twice(x int) int {"), "{check}");
    assert_eq!(std::fs::read_to_string(root.join("src/ugly.zl")).unwrap(), ugly);
    let done = c.ok("format", json!({ "path": "src/ugly.zl" }));
    assert!(done.starts_with("Formatted 1 file of 1"), "{done}");
    assert_eq!(std::fs::read_to_string(root.join("src/ugly.zl")).unwrap(), "namespace demo.models\n\nfunc Twice(x int) int {\n    return x * 2\n}\n");
    let again = c.ok("format", json!({ "path": "src/ugly.zl" }));
    assert!(again.starts_with("All 1 file is formatted"), "{again}");
    // The whole project: main.zl groups std imports apart.
    let all = c.ok("format", json!({ "apply": false }));
    assert!(all.starts_with("1 file of 3 would change") && all.contains("--- src/main.zl"), "{all}");
}

#[test]
fn answers_stdlib_questions() {
    let root = project();
    let mut c = Client::start(&root);
    let hits = c.ok("std_index", json!({ "query": "strings.Split", "limit": 3 }));
    assert!(hits.contains("function strings.Split: ") && hits.contains("strings/strings_extern.zl:"), "{hits}");
    assert!(!hits.contains(" C: "), "runtime notes are not documentation: {hits}");
    let all = c.ok("std_index", json!({}));
    assert!(all.contains("\nstrings (") && all.contains("\nio ("), "{all}");
    let doc = c.ok("doc", json!({ "symbol": "strings.Split" }));
    assert!(doc.starts_with("func Split(s string, sep string) []string"), "{doc}");
    let ns = c.ok("doc", json!({ "symbol": "demo.models" }));
    assert!(ns.contains("struct User") && ns.contains("A user of the system."), "{ns}");
    let hover = c.ok("hover", json!({ "symbol": "strings.Split" }));
    assert!(hover.contains("function in strings") && hover.contains("Example:"), "{hover}");
}

/// The `zell` binary next to this test's server, when cargo built it.
fn have_zell() -> bool {
    let exe = PathBuf::from(env!("CARGO_BIN_EXE_zell-mcp"));
    let zell = exe.with_file_name(format!("zell{}", std::env::consts::EXE_SUFFIX));
    let runtime = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../runtime/libzell_rt.a");
    if !zell.is_file() || !runtime.is_file() {
        eprintln!("skipping: needs {} (cargo build -p zell) and the runtime (make -C runtime)", zell.display());
        return false;
    }
    true
}

#[test]
fn runs_tests_builds_and_runs_through_the_cli() {
    if !have_zell() {
        return;
    }
    let root = project();
    std::fs::write(root.join("src/models_test.zl"), TESTS).unwrap();
    let mut c = Client::start(&root);
    let t = c.ok("run_tests", json!({}));
    assert!(t.starts_with("FAIL: 1 passed, 1 failed, 0 skipped"), "{t}");
    assert!(t.contains("--- FAIL: TestBroken"), "{t}");
    let only = c.ok("run_tests", json!({ "filter": "Hello$" }));
    assert!(only.starts_with("ok: 1 passed, 0 failed"), "{only}");
    let run = c.ok("run", json!({ "args": ["x"] }));
    assert!(run.starts_with("exit code 0\n--- stdout ---\nhi zell\n"), "{run}");
    assert!(c.ok("build", json!({})).starts_with("Build succeeded."));
    std::fs::write(root.join("src/main.zl"), MAIN.replace("u.Hello()", "u.Nope()")).unwrap();
    let (broken, err) = c.call("build", json!({}));
    assert!(err && broken.starts_with("Build failed (exit code 1)") && broken.contains("Nope"), "{broken}");
}
