//! std.log and std.diag: compile-time message-template checks (placeholder
//! count, literal templates, syntax), the lowering's laziness, `@log(skip)`,
//! sinks, context propagation, the W3C traceparent hooks of std.net.http
//! with the OTLP/JSON exporter, and CPU / heap profiles read back with
//! `go tool pprof` (skipped when `go` is not installed). The acceptance
//! programs are `examples/{log,metrics,tracing,cpuprofile}.sl` and
//! `tests/spec/stdlib/log_*`.

use slang::diagnostics::{Diagnostics, Severity};
use slang::driver;
use slang::span::SourceFile;
use std::path::{Path, PathBuf};
use std::process::Command;

fn analyze(src: &str) -> Vec<String> {
    let files = vec![SourceFile::new("l.sl".to_string(), src.to_string())];
    let mut diags = Diagnostics::new();
    let _ = driver::check(&files, &mut diags);
    diags.items.iter().filter(|d| d.severity == Severity::Error).map(|d| d.message.clone()).collect()
}

/// A program: namespace `t`, `import std.io` and `import std`, then `body`.
fn prog(body: &str) -> String {
    format!("namespace t\nimport std.io\nimport std\n{body}\n")
}

fn ok(body: &str) {
    // `prog` imports std.io for the programs that print; in the others it
    // is unused (the logger's output does not use the program's imports).
    let errors: Vec<String> = analyze(&prog(body)).into_iter().filter(|e| !e.starts_with("unused import std.io")).collect();
    assert!(errors.is_empty(), "unexpected errors: {errors:#?}");
}

fn err(body: &str, needle: &str) {
    let errors = analyze(&prog(body));
    assert!(errors.iter().any(|e| e.contains(needle)), "no error containing {needle:?} in {errors:#?}");
}

const LOGGER: &str = "var l = log.New().WriteTo.Console(log.ConsoleOptions{Stdout: true}).CreateLogger()";

#[test]
fn placeholder_count_must_match_arguments() {
    err(&format!("func main() {{\n    {LOGGER}\n    l.Info(\"user {{UserId}} paid {{Amount}}\", 7)\n}}"), "the template has 2 placeholders ({UserId}, {Amount}) but 1 argument is given");
    err(&format!("func main() {{\n    {LOGGER}\n    l.Warning(\"no holes\", 1, 2)\n}}"), "the template has 0 placeholders but 2 arguments are given");
    err("func main() {\n    log.Info(\"{A} {@B}\")\n}", "log.Info: the template has 2 placeholders ({A}, {@B}) but 0 arguments are given");
    ok(&format!("func main() {{\n    {LOGGER}\n    l.Info(\"{{A}} {{{{literal}}}} {{B:0.00}}\", 1, 2.5)\n}}"));
}

#[test]
fn template_must_be_a_valid_literal() {
    err(&format!("func main() {{\n    {LOGGER}\n    var t = \"x {{A}}\"\n    l.Info(t, 1)\n}}"), "the message template must be a string literal");
    err(&format!("func main() {{\n    {LOGGER}\n    l.Info(\"broken {{A\", 1)\n}}"), "unclosed '{'");
    err(&format!("func main() {{\n    {LOGGER}\n    l.Info(\"bad {{}}\")\n}}"), "invalid placeholder {}");
    err(&format!("func main() {{\n    {LOGGER}\n    l.Debug(\"{{A,x}}\", 1)\n}}"), "invalid alignment");
}

#[test]
fn error_first_argument_is_checked() {
    ok(&format!("func main() {{\n    {LOGGER}\n    var e = error(\"boom\")\n    l.Error(e, \"failed {{Id}}\", 3)\n}}"));
    err(&format!("func main() {{\n    {LOGGER}\n    l.Error(42, \"failed\")\n}}"), "cannot use");
}

#[test]
fn log_attribute_takes_skip() {
    err("struct S {\n    @log(hide: true) A int\n}\nfunc main() {\n}", "hide");
}

// ---- end to end -------------------------------------------------------------

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn ensure_runtime() {
    let lib = repo().join("runtime/libslang_rt.a");
    if !lib.is_file() {
        let st = Command::new("make").arg("-C").arg(repo().join("runtime")).status().expect("cannot run make");
        assert!(st.success(), "make -C runtime failed");
    }
}

fn tmp(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("slang-logdiag-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// Runs `src` with `slang run --leak-check` (plus `flags`) in `dir`: (exit
/// code, stdout, stderr).
fn run_in(dir: &Path, src: &str, flags: &[&str]) -> (i32, String, String) {
    ensure_runtime();
    let file = dir.join("main.sl");
    std::fs::write(&file, src).unwrap();
    let r = Command::new(env!("CARGO_BIN_EXE_slang")).arg("run").arg(&file).arg("--leak-check").args(flags).current_dir(dir).env("SLANG_NO_BACKTRACE", "1").output().unwrap();
    (r.status.code().unwrap_or(-1), String::from_utf8_lossy(&r.stdout).into_owned(), String::from_utf8_lossy(&r.stderr).into_owned())
}

#[test]
fn disabled_levels_do_not_evaluate_arguments_and_skip_works() {
    let dir = tmp("lazy");
    let src = prog(
        "struct Secret {\n    User string\n    @log(skip: true) Password string\n}\nvar n = 0\nfunc f() int {\n    n += 1\n    return n\n}\nfunc fixed() int {\n    return 0\n}\n\
         func main() {\n    var l = log.New().MinimumLevel(log.Level.Warning).Clock(fixed).WriteTo.Console(log.ConsoleOptions{Template: \"{Level:u3} {Message}{NewLine}\", Stdout: true}).CreateLogger()\n\
         \x20   l.Info(\"{X}\", f())\n    l.Debug(\"{X}\", f())\n    l.Warning(\"{X} {@S}\", f(), Secret{User: \"bob\", Password: \"hunter2\"})\n    println(n)\n}",
    );
    let (code, out, errs) = run_in(&dir, &src, &[]);
    let _ = std::fs::remove_dir_all(&dir);
    assert_eq!(code, 0, "{errs}");
    assert_eq!(out, "WRN 1 Secret { User: \"bob\" }\n1\n");
}

#[test]
fn global_logger_writes_to_stderr() {
    let dir = tmp("global");
    // (no std.io import: nothing here uses it, the logger's own output does
    // not count as a use of the program's imports)
    let src = "namespace t\nimport std\nfunc main() {\n    log.Info(\"hello {Name}\", \"x\")\n    log.Debug(\"hidden\")\n    log.Error(error(\"e\"), \"failed {N}\", 2)\n}\n";
    let (code, out, errs) = run_in(&dir, src, &[]);
    let _ = std::fs::remove_dir_all(&dir);
    assert_eq!(code, 0, "{errs}");
    assert_eq!(out, "");
    assert!(errs.starts_with("INFO hello x\nEROR failed 2\ne\n"), "{errs}");
}

#[test]
fn file_sink_rolls_by_size_and_retains() {
    let dir = tmp("roll");
    let src = prog(
        "func fixed() int {\n    return 1700000000000\n}\nasync func main() {\n\
         \x20   var l = log.New().Clock(fixed).WriteTo.File(\"logs/app-.log\", log.FileOptions{Template: \"{Message}{NewLine}\", RollingInterval: log.RollingInterval.Day, FileSizeLimit: 20, Retain: 2}).CreateLogger()\n\
         \x20   for i in 0..5 {\n        l.Info(\"line number {I}\", i)\n    }\n    await l.Close()\n\
         \x20   for f in try fs.ReadDir(\"logs\") else [] {\n        println(fs.Base(f), try io.ReadFile(\"logs/\" + fs.Base(f)) else \"?\")\n    }\n}",
    );
    let (code, out, errs) = run_in(&dir, &src, &[]);
    let _ = std::fs::remove_dir_all(&dir);
    assert_eq!(code, 0, "{errs}");
    assert_eq!(out, "app-20231114_003.log line number 3\n\napp-20231114_004.log line number 4\n\n");
}

/// Client span -> traceparent -> server span, exported as OTLP/JSON to a
/// collector served by the same program.
#[test]
fn traceparent_crosses_http_and_otlp_export_posts_json() {
    let dir = tmp("otlp");
    let src = "namespace t\nimport std.io\nimport std\nimport std.net\n\
        var body = \"\"\nvar header = \"\"\n\
        async func Hello(req *http.Request, res *http.Response) void! {\n    header = req.Header(\"traceparent\")\n    var s = diag.StartSpanKind(\"server\", diag.SpanKind.Server)\n    defer s.End()\n    res.Text(200, s.ParentSpanId)\n}\n\
        async func Collect(req *http.Request, res *http.Response) void! {\n    body = req.Body\n    res.Text(200, \"{}\")\n}\n\
        async func main() void! {\n    var r = http.NewRouter()\n    r.Get(\"/hello\", Hello)\n    r.Post(\"/v1/traces\", Collect)\n\
        \x20   var srv = http.Server{Addr: \"127.0.0.1:0\", Handler: r}\n    srv.Listen()?\n    var serving = srv.Serve()\n    var base = fmt.Format(\"http://127.0.0.1:{0}\", srv.Port())\n\
        \x20   var otlp = diag.NewOtlpExporter(base)\n    diag.AddExporter(otlp)\n    var root = diag.StartSpanKind(\"client\", diag.SpanKind.Client)\n\
        \x20   var a = await http.Get(base + \"/hello\")?\n    root.End()\n    var plain = await http.Get(base + \"/hello\")?\n    await diag.Shutdown()\n\
        \x20   println(a.Body == root.SpanId, header == \"\", plain.Body == \"\")\n    println(otlp.LastStatus, strings.Contains(body, \"\\\"resourceSpans\\\"\"), strings.Contains(body, \"\\\"spanId\\\":\\\"\" + root.SpanId + \"\\\"\"), strings.Contains(body, \"\\\"kind\\\":2\"))\n\
        \x20   await srv.Shutdown()\n    await serving?\n}\n";
    let (code, out, errs) = run_in(&dir, src, &[]);
    let _ = std::fs::remove_dir_all(&dir);
    assert_eq!(code, 0, "{errs}");
    assert_eq!(out, "true true true\n200 true true true\n");
}

fn have_go() -> bool {
    Command::new("go").arg("version").output().is_ok_and(|o| o.status.success())
}

fn pprof_top(path: &Path, extra: &[&str]) -> String {
    let o = Command::new(env!("CARGO_BIN_EXE_slang")).args(["tool", "pprof"]).args(extra).arg("-top").arg(path).output().unwrap();
    assert!(o.status.success(), "pprof failed: {}", String::from_utf8_lossy(&o.stderr));
    String::from_utf8_lossy(&o.stdout).into_owned()
}

#[test]
fn cpu_profile_is_readable_by_go_tool_pprof() {
    if !have_go() {
        eprintln!("skipping: go is not installed");
        return;
    }
    let dir = tmp("cpu");
    let src = prog(
        "func fib(n int) int {\n    if n < 2 {\n        return n\n    }\n    return fib(n - 1) + fib(n - 2)\n}\n\
         func spin(ms int) int {\n    var start = time.Now()\n    var acc = 0\n    for time.Since(start) < ms {\n        acc += fib(20)\n    }\n    return acc\n}\n\
         func main() void! {\n    diag.StartCPUProfile(\"cpu.pb\")?\n    var r = spin(300)\n    var n = diag.StopCPUProfile()?\n    println(n > 0, r > 0)\n}",
    );
    let (code, out, errs) = run_in(&dir, &src, &[]);
    assert_eq!(code, 0, "{errs}");
    assert_eq!(out, "true true\n");
    let top = pprof_top(&dir.join("cpu.pb"), &[]);
    let _ = std::fs::remove_dir_all(&dir);
    assert!(top.contains("Type: cpu"), "{top}");
    // Frame-pointer walks give the callers: fib is flat, spin and main cumulative.
    for f in ["t.fib", "t.spin", "t.main"] {
        assert!(top.contains(f), "{f} missing in:\n{top}");
    }
}

#[test]
fn heap_profile_lists_live_objects_by_type() {
    if !have_go() {
        eprintln!("skipping: go is not installed");
        return;
    }
    let dir = tmp("heap");
    let src = prog(
        "struct Node {\n    V int\n}\nfunc main() void! {\n    var keep []*Node = []\n    for i in 0..50 {\n        keep = append(keep, new Node{V: i})\n    }\n\
         \x20   diag.WriteHeapProfile(\"heap.pb\")?\n    println(strings.HasPrefix(diag.HeapProfile(), \"50 t.Node \"), len(keep))\n}",
    );
    let (code, out, errs) = run_in(&dir, &src, &[]);
    assert_eq!(code, 0, "{errs}");
    assert_eq!(out, "true 50\n");
    let top = pprof_top(&dir.join("heap.pb"), &["-sample_index=inuse_objects"]);
    let _ = std::fs::remove_dir_all(&dir);
    assert!(top.lines().any(|l| l.contains("t.Node") && l.trim_start().starts_with("50 ")), "{top}");
}
