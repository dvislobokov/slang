//! Semantic analysis tests: the example corpus plus focused unit tests.

use slang::diagnostics::{Diagnostics, Severity};
use slang::driver;
use slang::hir::*;
use slang::span::SourceFile;
use slang::types::Type;
use std::path::Path;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Analyses several in-memory files.
fn analyze(srcs: &[&str]) -> (Option<Program>, Vec<String>) {
    let files: Vec<SourceFile> = srcs.iter().enumerate().map(|(i, s)| SourceFile::new(format!("f{i}.sl"), *s)).collect();
    let mut diags = Diagnostics::new();
    let p = driver::check(&files, &mut diags);
    let errors = diags.items.iter().filter(|d| d.severity == Severity::Error).map(|d| d.message.clone()).collect();
    (p, errors)
}

fn ok_files(srcs: &[&str]) -> Program {
    let (p, errors) = analyze(srcs);
    assert!(errors.is_empty(), "unexpected errors: {errors:#?}");
    p.expect("program")
}

fn ok(src: &str) -> Program {
    ok_files(&[src])
}

/// Expects an error whose message contains `needle`.
fn err_files(srcs: &[&str], needle: &str) {
    let (p, errors) = analyze(srcs);
    assert!(p.is_none(), "expected failure");
    assert!(errors.iter().any(|e| e.contains(needle)), "no error containing {needle:?} in {errors:#?}");
}

fn err(src: &str, needle: &str) {
    err_files(&[src], needle)
}

/// `namespace t` + `import std.io` + body.
fn prog(body: &str) -> String {
    format!("namespace t\nimport std.io\nimport std\n{body}\n")
}

/// A `main` with the given statements.
fn main_body(stmts: &str) -> String {
    prog(&format!("func main() {{\n{stmts}\n}}"))
}

fn func<'p>(p: &'p Program, name: &str) -> &'p Function {
    p.funcs.iter().find(|f| f.name == name).unwrap_or_else(|| panic!("no function {name}"))
}

fn examples_dir() -> &'static Path {
    Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/../examples"))
}

fn check_path(path: &Path) -> (Option<Program>, Vec<String>) {
    let paths = driver::collect_files(path).unwrap();
    let files = driver::load(&paths).unwrap();
    let mut diags = Diagnostics::new();
    let p = driver::check(&files, &mut diags);
    let errors = diags.items.iter().filter(|d| d.severity == Severity::Error).map(|d| d.message.clone()).collect();
    (p, errors)
}

/// Walks all expressions of a block (shallow helper for assertions).
fn exprs_in(b: &Block, out: &mut Vec<Expr>) {
    for s in &b.stmts {
        match &s.kind {
            StmtKind::Let { init: Some(e), .. } | StmtKind::Expr(e) | StmtKind::Discard(e) | StmtKind::Defer(e) => out.push(e.clone()),
            StmtKind::LetTuple { init, .. } => out.push(init.clone()),
            StmtKind::Assign { target, value } | StmtKind::CompoundAssign { target, value, .. } => {
                out.push(target.clone());
                out.push(value.clone());
            }
            StmtKind::Return(ReturnValue::Value(e) | ReturnValue::Error(e)) => out.push(e.clone()),
            StmtKind::If { cond, then, else_ } => {
                out.push(cond.clone());
                exprs_in(then, out);
                if let Some(e) = else_ {
                    exprs_in(e, out);
                }
            }
            StmtKind::Block(b) | StmtKind::Loop { body: b, .. } | StmtKind::ForRange { body: b, .. } | StmtKind::ForSlice { body: b, .. } | StmtKind::ForString { body: b, .. } | StmtKind::ForMap { body: b, .. } => exprs_in(b, out),
            StmtKind::Switch(sw) => {
                if let Some((_, e)) = &sw.subject {
                    out.push(e.clone());
                }
                for c in &sw.cases {
                    exprs_in(&c.body, out);
                }
            }
            _ => {}
        }
    }
}

fn main_exprs(p: &Program) -> Vec<Expr> {
    let mut v = Vec::new();
    exprs_in(&func(p, "main").body, &mut v);
    v
}

// ---------------------------------------------------------------------------
// Example corpus
// ---------------------------------------------------------------------------

#[test]
fn every_example_checks_cleanly() {
    let mut count = 0;
    for entry in std::fs::read_dir(examples_dir()).unwrap() {
        let path = entry.unwrap().path();
        if path.extension().is_some_and(|e| e == "sl") {
            let (p, errors) = check_path(&path);
            assert!(errors.is_empty(), "{}: {errors:#?}", path.display());
            let p = p.unwrap();
            assert!(p.entry.is_some(), "{}: no entry point", path.display());
            count += 1;
        }
    }
    assert!(count >= 14, "only {count} examples found");
}

#[test]
fn multi_file_example_checks_cleanly() {
    let (p, errors) = check_path(&examples_dir().join("multi"));
    assert!(errors.is_empty(), "{errors:#?}");
    let p = p.unwrap();
    let entry = &p.funcs[p.entry.unwrap().0 as usize];
    assert_eq!(entry.full_name, "app.main");
    assert!(p.structs.iter().any(|s| s.full_name == "app.models.User"));
}

/// Each compile-failure example and a key phrase its diagnostic must contain
/// (derived from the file's `// error:` line).
const ERROR_KEYWORDS: &[(&str, &str)] = &[
    ("await_outside_async.sl", "await outside async"),
    ("catch_falls_through.sl", "catch block must return"),
    ("missing_return.sl", "missing return"),
    ("question_outside_fallible.sl", "? outside of fallible function"),
    ("type_mismatch.sl", "cannot assign string to int"),
    ("unhandled_result.sl", "result of type int! must be handled"),
    ("unhandled_void_result.sl", "result of type void! must be handled"),
    ("unknown_ident.sl", "unknown identifier undefinedName"),
    ("unused_var.sl", "unused variable x"),
];

#[test]
fn every_compile_error_example_fails_with_its_diagnostic() {
    let dir = examples_dir().join("errors_compile");
    let mut files: Vec<String> = std::fs::read_dir(&dir).unwrap().map(|e| e.unwrap().file_name().into_string().unwrap()).filter(|n| n.ends_with(".sl")).collect();
    files.sort();
    let mapped: Vec<&str> = ERROR_KEYWORDS.iter().map(|(f, _)| *f).collect();
    assert_eq!(files, mapped, "ERROR_KEYWORDS must list every errors_compile file");
    for (file, keyword) in ERROR_KEYWORDS {
        let path = dir.join(file);
        let first_line = std::fs::read_to_string(&path).unwrap().lines().next().unwrap().to_string();
        assert!(first_line.starts_with("// error:"), "{file}");
        let (p, errors) = check_path(&path);
        assert!(p.is_none(), "{file} unexpectedly compiled");
        assert!(errors.iter().any(|e| e.contains(keyword)), "{file}: no error containing {keyword:?}: {errors:#?}");
    }
}

// ---------------------------------------------------------------------------
// Imports, namespaces, visibility
// ---------------------------------------------------------------------------

#[test]
fn import_rules_follow_the_table() {
    ok("namespace t\nfunc main() { std.io.println(\"hi\") }");
    ok("namespace t\nimport std\nfunc main() { io.println(\"hi\") }");
    ok("namespace t\nimport std.io\nfunc main() { println(\"hi\") }");
    ok("namespace t\nimport std.io as out\nfunc main() { out.println(\"hi\") }");
    err("namespace t\nfunc main() { println(\"hi\") }", "unknown identifier println");
    err("namespace t\nimport std\nfunc main() { println(\"hi\") }", "unknown identifier println");
    err("namespace t\nimport std.io as out\nfunc main() { println(\"hi\") }", "unknown identifier println");
    err("namespace t\nimport std.nope\nfunc main() {}", "unknown namespace");
}

#[test]
fn namespaces_merge_across_files_and_type_imports_work() {
    let a = "namespace app.models\nstruct User { Name string }";
    let b = "namespace app.models\nfunc Make(n string) User { return User{Name: n} }";
    let c = "namespace app\nimport std.io\nimport app.models.User\nfunc main() { var u User = app.models.Make(\"x\")\n println(u.Name) }";
    ok_files(&[a, b, c]);
    let c2 = "namespace app\nimport std.io\nimport app.models.User as U\nfunc main() { var u = U{Name: \"y\"}\n println(u.Name) }";
    ok_files(&[a, b, c2]);
}

#[test]
fn ambiguous_short_names_are_errors_on_use() {
    let a = "namespace a\nfunc F() int { return 1 }";
    let b = "namespace b\nfunc F() int { return 2 }";
    let c = "namespace app\nimport a\nimport b\nimport std.io\nfunc main() { println(F()) }";
    err_files(&[a, b, c], "ambiguous name F");
    let c2 = "namespace app\nimport a\nimport b as bb\nimport std.io\nfunc main() { println(F(), bb.F()) }";
    ok_files(&[a, b, c2]);
}

#[test]
fn private_names_are_hidden_from_other_namespaces() {
    let m = "namespace m\nstruct S { Pub int\n priv int\n func get() int { return this.priv } }\nfunc helper() int { return 1 }";
    err_files(&[m, "namespace app\nimport m\nfunc main() { var s = m.S{}\n _ = s.priv }"], "priv is private to namespace m");
    err_files(&[m, "namespace app\nimport m\nfunc main() { var s = S{priv: 1}\n _ = s }"], "private to namespace m");
    err_files(&[m, "namespace app\nfunc main() { _ = m.helper() }"], "helper is private");
    err_files(&[m, "namespace app\nimport m\nfunc main() { var s = S{}\n _ = s.get() }"], "get is private");
    ok_files(&[m, "namespace app\nimport m\nfunc main() { var s = S{Pub: 1}\n _ = s.Pub }"]);
}

#[test]
fn duplicate_declarations_are_reported() {
    err(&prog("func F() {}\nfunc F() {}\nfunc main() {}"), "duplicate declaration of F");
    err_files(&["namespace a\nconst X = 1", "namespace a\nconst X = 2"], "duplicate declaration of X");
    err(&prog("struct S { A int\n A string }\nfunc main() {}"), "duplicate field A");
    err(&main_body("var x = 1\nvar x = 2\nprintln(x)"), "x redeclared");
}

#[test]
fn unknown_types_and_namespaces() {
    err(&prog("func F(x Foo) {}\nfunc main() {}"), "unknown type Foo");
    err(&main_body("var x = nope.Thing()\nprintln(x)"), "unknown identifier nope");
    err(&main_body("std.io.nothing()"), "unknown identifier nothing in namespace std.io");
}

// ---------------------------------------------------------------------------
// Types and expressions
// ---------------------------------------------------------------------------

#[test]
fn untyped_constants_adapt_to_context() {
    let p = ok(&prog("func Half(x float) float { return x / 2 }\nconst K = 3\nfunc main() { var f float = K\n var g = Half(1)\n println(f, g, 1 << 3) }"));
    let main = func(&p, "main");
    let StmtKind::Let { init: Some(e), .. } = &main.body.stmts[0].kind else { panic!() };
    assert!(matches!(e.kind, ExprKind::Const(ConstValue::Float(f)) if f == 3.0));
    assert_eq!(e.ty, p.types.float);
    let half = func(&p, "Half");
    let StmtKind::Return(ReturnValue::Value(e)) = &half.body.stmts[0].kind else { panic!() };
    let ExprKind::Binary { rhs, .. } = &e.kind else { panic!() };
    assert!(matches!(rhs.kind, ExprKind::Const(ConstValue::Float(_))));
    err(&main_body("var i = 1\nvar f = 2.5\nprintln(i + f)"), "mismatched types int and float");
    err(&main_body("var i int = 2.5\nprintln(i)"), "cannot assign float to variable of type int");
    ok(&main_body("var i int = 2.0\nvar c = int(3.99)\nprintln(i, c)"));
}

#[test]
fn type_mismatches_in_calls_returns_and_binops() {
    err(&prog("func F(a int) {}\nfunc main() { F(\"x\") }"), "cannot use string as int in argument 1 of F");
    err(&prog("func F(a int) {}\nfunc main() { F(1, 2) }"), "wrong number of arguments in call to F: expected 1, got 2");
    err(&prog("func F() int { return \"s\" }\nfunc main() {}"), "cannot return string");
    err(&main_body("var s = \"a\" - \"b\"\nprintln(s)"), "operator - is not defined on string");
    err(&main_body("var b = 1 && true\nprintln(b)"), "requires bool operands");
}

#[test]
fn nil_needs_a_nilable_type() {
    err(&main_body("var x int = nil\nprintln(x)"), "cannot use nil as int");
    err(&main_body("var x = nil\n_ = x"), "untyped nil");
    let p = ok(&prog("struct N { V int }\nfunc main() { var p *N = nil\n println(p == nil) }"));
    assert!(main_exprs(&p).iter().any(|e| matches!(&e.kind, ExprKind::Print(items) if items.iter().any(|i| matches!(i.arg.as_ref().map(|a| &a.kind), Some(ExprKind::IsNil { .. }))))));
}

#[test]
fn strings_have_distinct_ops() {
    let p = ok(&main_body("var a = \"x\"\nvar b = a + \"y\"\nprintln(b < a, b == a)"));
    let es = main_exprs(&p);
    let StmtKind::Let { init: Some(e), .. } = &func(&p, "main").body.stmts[1].kind else { panic!() };
    assert!(matches!(e.kind, ExprKind::Binary { op: BinOp::StrConcat, .. }));
    let ExprKind::Print(items) = &es.last().unwrap().kind else { panic!() };
    let ops: Vec<BinOp> = items.iter().filter_map(|i| match &i.arg.as_ref()?.kind {
        ExprKind::Binary { op, .. } => Some(*op),
        _ => None,
    }).collect();
    assert_eq!(ops, vec![BinOp::StrLt, BinOp::StrEq]);
}

#[test]
fn println_is_lowered_per_type() {
    let p = ok(&main_body("println(1, 2.5, true, 'c', \"s\", [1, 2])"));
    let ExprKind::Print(items) = &main_exprs(&p)[0].kind else { panic!() };
    let ops: Vec<PrintOp> = items.iter().map(|i| i.op).collect();
    use PrintOp::*;
    assert_eq!(ops, vec![Int, Space, Float, Space, Bool, Space, Char, Space, Str, Space, Str, Newline]);
    assert!(matches!(items[10].arg.as_ref().unwrap().kind, ExprKind::ToString(_)));
    err(&prog("struct S {}\nfunc main() { println(S{}) }"), "cannot be printed");
}

// ---------------------------------------------------------------------------
// Structs, methods, interfaces
// ---------------------------------------------------------------------------

#[test]
fn methods_take_this_pointer_and_receivers_are_implicitly_addressed() {
    let p = ok(&prog("struct C { n int\n func Inc() { this.n += 1 }\n func Get() int { return this.n } }\nfunc main() { var c = C{}\n c.Inc()\n var p = &c\n p.Inc()\n println(c.Get()) }"));
    let inc = func(&p, "Inc");
    assert_eq!(inc.params.len(), 1);
    let this = &inc.locals[0];
    assert_eq!(this.kind, LocalKind::This);
    assert!(matches!(p.types.get(this.ty), Type::Pointer(_)));
    let main = func(&p, "main");
    let StmtKind::Expr(call) = &main.body.stmts[1].kind else { panic!() };
    let ExprKind::Call { args, .. } = &call.kind else { panic!() };
    assert!(matches!(args[0].kind, ExprKind::AddrOf(_)));
    let StmtKind::Expr(call) = &main.body.stmts[3].kind else { panic!() };
    let ExprKind::Call { args, .. } = &call.kind else { panic!() };
    assert!(matches!(args[0].kind, ExprKind::Local(_)), "pointer receiver passed as is");
    assert!(main.locals[0].address_taken);
    err(&main_body("var x = 1\nx.Foo()"), "cannot call method Foo on value of type int");
    err(&main_body("println(this)"), "this used outside of a method");
    err(&prog("struct S { static func New() S { return this } }\nfunc main() {}"), "this used outside of a method");
}

#[test]
fn auto_deref_and_embedding() {
    let p = ok(&prog("struct A { Name string\n func Hi() string { return this.Name } }\nstruct D { A\n Age int }\nfunc main() { var d = new D{A: A{Name: \"x\"}}\n println(d.Name, d.Hi(), d.A.Name) }"));
    let ExprKind::Print(items) = &main_exprs(&p)[1].kind else { panic!() };
    let ExprKind::Field { base, index: 0 } = &items[0].arg.as_ref().unwrap().kind else { panic!("{:?}", items[0]) };
    let ExprKind::Field { base, index: 0 } = &base.kind else { panic!() };
    assert!(matches!(base.kind, ExprKind::Deref(_)));
}

#[test]
fn struct_literal_checks() {
    err(&prog("struct P { X int }\nfunc main() { var p = P{Y: 1}\n _ = p }"), "unknown field Y in struct literal");
    err(&prog("struct P { X int }\nfunc main() { var p = P{X: \"s\"}\n _ = p }"), "cannot use string as int for field X");
}

#[test]
fn structural_interfaces() {
    let src = prog("interface Shape { func Area() float }\nstruct Sq { S float\n func Area() float { return this.S * this.S } }\nstruct Bad { func Area() int { return 1 } }\nfunc Use(s Shape) float { return s.Area() }");
    let p = ok(&format!("{src}\nfunc main() {{ var q = Sq{{S: 2}}\n println(Use(q), Use(&q)) }}"));
    let ExprKind::Print(items) = &main_exprs(&p)[1].kind else { panic!() };
    let ExprKind::Call { args, .. } = &items[0].arg.as_ref().unwrap().kind else { panic!() };
    assert!(matches!(args[0].kind, ExprKind::ToInterface { .. }));
    let uses = func(&p, "Use");
    let StmtKind::Return(ReturnValue::Value(e)) = &uses.body.stmts[0].kind else { panic!() };
    assert!(matches!(e.kind, ExprKind::Call { callee: Callee::Interface { method: 0, .. }, .. }));
    err(&format!("{src}\nfunc main() {{ _ = Use(Bad{{}}) }}"), "Bad does not implement t.Shape: method Area has type func() int, want func() float");
    err(&format!("{src}\nstruct None {{}}\nfunc main() {{ _ = Use(None{{}}) }}"), "missing method Area");
}

#[test]
fn errors_as_interfaces() {
    ok(&prog("struct E { func Message() string { return \"e\" } }\nfunc F() int! { return E{} }\nfunc main() { var x = try F() else 0\n println(x) }"));
    let p = ok(&prog("struct E { func Message() string { return \"e\" } }\nfunc F() int! { return E{} }\nfunc main() { _ = F() }"));
    let StmtKind::Return(ReturnValue::Error(e)) = &func(&p, "F").body.stmts[0].kind else { panic!() };
    assert!(matches!(e.kind, ExprKind::ToInterface { .. }));
    assert_eq!(e.ty, p.types.error);
}

// ---------------------------------------------------------------------------
// Errors: ?, try, unhandled results
// ---------------------------------------------------------------------------

#[test]
fn propagate_typing() {
    let p = ok(&prog("func P(s string) int! { return strings.ToInt(s) ? }\nfunc Q(s string) int! { var n = P(s)?(\"ctx\")\n return n + 1 }\nfunc main() { _ = Q(\"1\") }"));
    let q = func(&p, "Q");
    let StmtKind::Let { init: Some(e), .. } = &q.body.stmts[0].kind else { panic!() };
    let ExprKind::Propagate { value, context } = &e.kind else { panic!() };
    assert_eq!(e.ty, p.types.int);
    assert!(matches!(p.types.get(value.ty), Type::Fallible(_)));
    assert!(context.is_some());
    err(&prog("func P() int! { return 1 }\nfunc main() { var x = P()?\n println(x) }"), "? outside of fallible function");
    err(&prog("func P() int { return 1 }\nfunc Q() int! { return P()? }\nfunc main() {}"), "? requires a fallible");
    err(&prog("func P() int! { return 1 }\nfunc main() { P() }"), "result of type int! must be handled");
    err(&prog("func P() int! { return 1 }\nfunc main() { println(P() + 1) }"), "result of type int! must be handled");
    ok(&prog("func P() void! { }\nfunc Q() void! { P()?\n return error(\"x\") }\nfunc main() { _ = Q() }"));
}

#[test]
fn try_else_and_catch_typing() {
    let p = ok(&prog("func P() int! { return 1 }\nfunc main() { var a = try P() else 7\n for i in 0..3 { var b = try P() else continue\n println(i, b) }\n println(a) }"));
    let StmtKind::Let { init: Some(e), .. } = &func(&p, "main").body.stmts[0].kind else { panic!() };
    let ExprKind::TryElse { fallback, .. } = &e.kind else { panic!() };
    assert!(matches!(**fallback, TryFallback::Value(_)));
    assert_eq!(e.ty, p.types.int);
    err(&prog("func P() int! { return 1 }\nfunc main() { var a = try P() else \"s\"\n println(a) }"), "expected int, found string");
    err(&prog("func P() int { return 1 }\nfunc main() { var a = try P() else 0\n println(a) }"), "try requires a fallible");
    // catch must diverge only when the value is used
    ok(&prog("func P() int! { return 1 }\nfunc main() { try P() catch err { println(err) } }"));
    err(&prog("func P() int! { return 1 }\nfunc main() { var a = try P() catch err { println(err) }\n println(a) }"), "catch block must return");
    ok(&prog("func P() int! { return 1 }\nfunc main() { for { var a = try P() catch _ { break }\n println(a) } }"));
}

// ---------------------------------------------------------------------------
// Control flow
// ---------------------------------------------------------------------------

#[test]
fn missing_return_follows_go_terminating_rules() {
    ok(&prog("func F(n int) int { if n > 0 { return 1 } else { return 2 } }\nfunc main() {}"));
    ok(&prog("func F(n int) int { for { } }\nfunc main() {}"));
    ok(&prog("func F(n int) int { switch n { case 1: return 1\n default: return 2 } }\nfunc main() {}"));
    ok(&prog("func F(n int) int { panic(\"no\") }\nfunc main() {}"));
    ok(&prog("func F(n int) void! { if n > 0 { return error(\"x\") } }\nfunc main() {}"));
    err(&prog("func F(n int) int { switch n { case 1: return 1 } }\nfunc main() {}"), "missing return");
    err(&prog("func F(n int) int { for { break } }\nfunc main() {}"), "missing return");
    err(&prog("func F(n int) int { }\nfunc main() {}"), "missing return");
}

#[test]
fn unused_variables_and_undeclared_assignment() {
    err(&main_body("var y = 2\ny = 3"), "unused variable y");
    err(&main_body("for i in 0..3 { }"), "unused variable i");
    ok(&main_body("for _ in 0..3 { }\nvar f = func() int { return 1 }\n_ = f"));
    err(&main_body("z = 3"), "cannot assign to undeclared variable z");
}

#[test]
fn await_needs_async() {
    let p = ok(&prog("async func F() int { await task.Delay(1)\n return 1 }\nasync func main() { var x = await F()\n println(x) }"));
    let f = func(&p, "F");
    assert!(f.is_async);
    assert_eq!(f.ret, p.types.int);
    err(&prog("async func F() int { return 1 }\nfunc main() { var x = await F()\n println(x) }"), "await outside async");
    err(&prog("func F() int { return 1 }\nasync func main() { var x = await F()\n println(x) }"), "await requires a Task");
}

#[test]
fn for_loops_lower_to_specific_nodes() {
    let p = ok(&main_body("for i in 0..3 { println(i) }\nfor i, x in [1, 2] { println(i, x) }\nfor c in \"ab\" { println(c) }\nfor k, v in map[string]int{} { println(k, v) }"));
    let kinds: Vec<&str> = func(&p, "main").body.stmts.iter().map(|s| match &s.kind {
        StmtKind::ForRange { .. } => "range",
        StmtKind::ForSlice { .. } => "slice",
        StmtKind::ForString { .. } => "string",
        StmtKind::ForMap { .. } => "map",
        _ => "other",
    }).collect();
    assert_eq!(kinds, vec!["range", "slice", "string", "map"]);
    let StmtKind::ForString { ch: Some(ch), .. } = &func(&p, "main").body.stmts[2].kind else { panic!() };
    assert_eq!(func(&p, "main").locals[ch.0 as usize].ty, p.types.char_);
}

#[test]
fn switch_typing() {
    let p = ok(&prog("func F(n int) string { switch n { case 1, 2: return \"a\"\n case 3..10: return \"b\"\n case n > 100: return \"c\"\n default: return \"d\" } }\nfunc main() {}"));
    let StmtKind::Switch(sw) = &func(&p, "F").body.stmts[0].kind else { panic!() };
    assert_eq!(sw.cases[0].patterns.len(), 2);
    assert!(sw.cases.iter().all(|c| c.patterns.iter().all(|p| matches!(p, Pattern::Cond(_)))));
    err(&prog("func F(n int) { switch n { case \"x\": return } }\nfunc main() {}"), "switch pattern type mismatch");
    err(&prog("func F(n string) { switch n { case 1..3: return } }\nfunc main() {}"), "range pattern needs a numeric");
    // type switch
    let src = prog("interface I { func M() int }\nstruct A { func M() int { return 1 } }\nstruct B { }\n");
    ok(&format!("{src}\nfunc F(i I) int {{ switch i {{ case A a: return a.M()\n default: return 0 }} }}\nfunc main() {{}}"));
    err(&format!("{src}\nfunc F(i I) int {{ switch i {{ case B b: return 1 }}\n return 0 }}\nfunc main() {{}}"), "impossible type switch case");
}

#[test]
fn enum_payloads_and_matching() {
    let src = prog("enum R { Ok(v int)\n Fail(code int, why string)\n Empty }\nenum C { Red, Green }");
    let p = ok(&format!("{src}\nfunc F(r R) int {{ switch r {{ case R.Ok(v): return v\n case R.Fail(c, _): return c\n case R.Empty: return 0 }}\n return -1 }}\nfunc main() {{ println(F(R.Ok(1)), F(R.Empty), int(C.Green), C.Red == C.Green, C.Red) }}"));
    let StmtKind::Switch(sw) = &func(&p, "F").body.stmts[0].kind else { panic!() };
    let Pattern::Variant { variant: 1, bindings } = &sw.cases[1].patterns[0] else { panic!() };
    assert!(bindings[0].is_some() && bindings[1].is_none());
    assert!(p.enums.iter().any(|e| e.name == "C" && e.is_plain));
    err(&format!("{src}\nfunc main() {{ var r = R.Ok(\"s\")\n _ = r }}"), "cannot use string as int");
    err(&format!("{src}\nfunc main() {{ var r = R.Ok\n _ = r }}"), "has fields");
    err(&format!("{src}\nfunc F(r R) {{ switch r {{ case R.Fail(x): return }} }}\nfunc main() {{}}"), "has 2 field(s)");
    err(&format!("{src}\nfunc F(r R) {{ switch r {{ case C.Red: return }} }}\nfunc main() {{}}"), "switch pattern type mismatch");
}

#[test]
fn closures_capture_by_reference() {
    let p = ok(&prog("func Counter() func() int { var n = 0\n return func() int { n += 1\n return n } }\nfunc main() { println(Counter()()) }"));
    let c = func(&p, "Counter");
    assert!(c.locals[0].captured);
    let lambda = p.funcs.iter().find(|f| matches!(f.kind, FuncKind::Lambda { .. })).unwrap();
    assert_eq!(lambda.captures.len(), 1);
    assert_eq!(lambda.captures[0].source, CaptureSource::Local(LocalId(0)));
}

#[test]
fn defer_spills_arguments() {
    let p = ok(&main_body("var x = 1\ndefer println(\"x\", x)\nx = 2\nprintln(x)"));
    let stmts = &func(&p, "main").body.stmts;
    assert!(matches!(stmts[1].kind, StmtKind::Let { .. }), "argument spilled");
    let StmtKind::Defer(e) = &stmts[2].kind else { panic!() };
    let ExprKind::Closure { captures, .. } = &e.kind else { panic!() };
    assert_eq!(captures.len(), 1);
}

#[test]
fn generics_are_rejected() {
    err(&prog("func Id<T>(x T) T { return x }\nfunc main() {}"), "generics are not supported yet");
    err(&prog("struct Box<T> { V T }\nfunc main() {}"), "generics are not supported yet");
    err(&prog("func F(l List<int>) {}\nfunc main() {}"), "generics are not supported yet");
    err(&main_body("var x = Id<int>(1)\n_ = x"), "generics are not supported yet");
}

#[test]
fn attributes_are_resolved_and_evaluated() {
    let src = prog(
        "@attribute(targets: [Target.Field])\nstruct range { min int\n max int\n message string = \"out of range\" }\n\
         @json(name: \"user\")\nstruct User { @range(min: 0, max: 150) Age int\n @range(1, 64) Name string\n @deprecated(\"use Y\")\n func X() {} }\nfunc main() {}",
    );
    let p = ok(&src);
    let user = p.structs.iter().find(|s| s.name == "User").unwrap();
    assert_eq!(user.attrs[0].kind, AttrKind::Builtin("json".into()));
    let age = &user.fields[0];
    assert!(matches!(age.attrs[0].kind, AttrKind::User(_)));
    assert_eq!(age.attrs[0].args, vec![("min".into(), ConstValue::Int(0)), ("max".into(), ConstValue::Int(150)), ("message".into(), ConstValue::Str("out of range".into()))]);
    err(&prog("@nope\nstruct S {}\nfunc main() {}"), "unknown attribute @nope");
    err(&prog("@attribute(targets: [Target.Field])\nstruct r { min int }\n@r(1)\nstruct S {}\nfunc main() {}"), "cannot be applied to a struct");
    err(&prog("@attribute\nstruct r { min int }\nstruct S { @r(\"x\") A int }\nfunc main() {}"), "attribute argument has type string, want int");
    err(&prog("@attribute\nstruct r { min int }\nstruct S { @r() A int }\nfunc main() {}"), "missing argument min");
    err(&prog("@attribute\nstruct r { min int }\nstruct S { @r(1) @r(2) A int }\nfunc main() {}"), "not repeatable");
}

#[test]
fn diagnostics_carry_file_index() {
    let files = vec![SourceFile::new("a.sl", "namespace a\nfunc F() {}"), SourceFile::new("b.sl", "namespace b\nfunc G() { nope() }")];
    let mut diags = Diagnostics::new();
    assert!(driver::check(&files, &mut diags).is_none());
    assert_eq!(diags.items[0].file, 1);
    assert!(diags.render_all_files(&files).contains("b.sl:2"));
}

#[test]
fn hir_dump_is_readable() {
    let p = ok(&main_body("println(\"hi\")"));
    let d = p.dump();
    assert!(d.contains("func #0 t.main"), "{d}");
    assert!(d.contains("print[Str \"hi\":string; Newline]"), "{d}");
}
