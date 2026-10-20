//! Integration tests for the lexer + parser.

use slang::ast::*;
use slang::diagnostics::{Diagnostic, Diagnostics, Severity};
use slang::span::SourceFile;
use slang::{lexer, parser};

const DESIGN: &str = include_str!("../../DESIGN.md");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn try_parse(src: &str) -> (Option<File>, Diagnostics) {
    let mut diags = Diagnostics::new();
    let tokens = lexer::lex(src, &mut diags);
    let file = parser::parse(tokens, &mut diags);
    (file, diags)
}

fn parse_ok(src: &str) -> File {
    let (file, diags) = try_parse(src);
    let errors: Vec<_> = diags.items.iter().filter(|d| d.severity == Severity::Error).collect();
    if !errors.is_empty() || file.is_none() {
        let sf = SourceFile::new("test.sl", src);
        panic!("unexpected errors:\n{}\n--- source ---\n{src}", diags.render_all(&sf));
    }
    file.unwrap()
}

/// Parses and expects at least one error; returns all diagnostics.
fn parse_err(src: &str) -> Vec<Diagnostic> {
    let (file, diags) = try_parse(src);
    assert!(diags.has_errors(), "expected errors for:\n{src}");
    assert!(file.is_none());
    diags.items
}

/// Statements of `func f() { <body> }`.
fn stmts(body: &str) -> Vec<Stmt> {
    let file = parse_ok(&format!("namespace t\nfunc f() {{\n{body}\n}}\n"));
    match file.decls.into_iter().next() {
        Some(Decl::Func(f)) => f.body.unwrap().stmts,
        other => panic!("expected func, got {other:?}"),
    }
}

fn stmt(body: &str) -> StmtKind {
    let mut s = stmts(body);
    assert_eq!(s.len(), 1, "expected one statement in {body:?}: {s:#?}");
    s.remove(0).kind
}

/// The expression of `var x = <src>`.
fn expr(src: &str) -> Expr {
    match stmt(&format!("var x = {src}")) {
        StmtKind::Var(v) => v.init.unwrap(),
        other => panic!("{other:?}"),
    }
}

fn decls(src: &str) -> Vec<Decl> {
    parse_ok(&format!("namespace t\n{src}\n")).decls
}

/// The type of `var x <src>`.
fn ty(src: &str) -> TypeExpr {
    match stmt(&format!("var x {src}")) {
        StmtKind::Var(v) => v.ty.unwrap(),
        other => panic!("{other:?}"),
    }
}

/// Compact rendering of expressions for precedence assertions.
fn show(e: &Expr) -> String {
    match &e.kind {
        ExprKind::Int(v) => v.to_string(),
        ExprKind::Float(v) => format!("{v:?}"),
        ExprKind::Str(s) => format!("{s:?}"),
        ExprKind::Char(c) => format!("{c:?}"),
        ExprKind::Bool(b) => b.to_string(),
        ExprKind::Nil => "nil".into(),
        ExprKind::This => "this".into(),
        ExprKind::Ident(i) => i.name.clone(),
        ExprKind::Field { base, name } => format!("{}.{}", show(base), name.name),
        ExprKind::Call { callee, args } => {
            format!("{}({})", show(callee), args.iter().map(show).collect::<Vec<_>>().join(", "))
        }
        ExprKind::Index { base, index } => format!("{}[{}]", show(base), show(index)),
        ExprKind::Unary { op, expr } => {
            let o = match op {
                UnaryOp::Neg => "-",
                UnaryOp::Not => "!",
                UnaryOp::AddrOf => "&",
                UnaryOp::Deref => "*",
            };
            format!("({o}{})", show(expr))
        }
        ExprKind::Binary { op, left, right } => format!("({} {op:?} {})", show(left), show(right)),
        ExprKind::Propagate { expr, context: None } => format!("{}?", show(expr)),
        ExprKind::Propagate { expr, context: Some(c) } => format!("{}?({})", show(expr), show(c)),
        ExprKind::Try { expr, .. } => format!("try {}", show(expr)),
        ExprKind::Await(e) => format!("(await {})", show(e)),
        ExprKind::StructLit { path, generics, fields } => format!(
            "{}{}{{{}}}",
            path.to_string(),
            if generics.is_empty() { String::new() } else { format!("<{}>", generics.len()) },
            fields.iter().map(|f| format!("{}: {}", f.name.name, show(&f.value))).collect::<Vec<_>>().join(", ")
        ),
        ExprKind::TypeArgs { base, args } => format!("{}<{}>", show(base), args.len()),
        ExprKind::New(e) => format!("new {}", show(e)),
        ExprKind::SliceLit(es) => format!("[{}]", es.iter().map(show).collect::<Vec<_>>().join(", ")),
        ExprKind::MapLit { entries, .. } => format!(
            "map{{{}}}",
            entries.iter().map(|(k, v)| format!("{}: {}", show(k), show(v))).collect::<Vec<_>>().join(", ")
        ),
        ExprKind::Tuple(es) => format!("tuple({})", es.iter().map(show).collect::<Vec<_>>().join(", ")),
        ExprKind::Range(a, b) => format!(
            "{}..{}",
            a.as_deref().map(show).unwrap_or_default(),
            b.as_deref().map(show).unwrap_or_default()
        ),
        ExprKind::Lambda { params, .. } => format!("lambda/{}", params.len()),
        ExprKind::Paren(e) => format!("[{}]", show(e)),
    }
}

fn named(t: &TypeExpr) -> String {
    match &t.kind {
        TypeKind::Named { path, generics } if generics.is_empty() => path.to_string(),
        TypeKind::Named { path, generics } => {
            format!("{}<{}>", path.to_string(), generics.iter().map(named).collect::<Vec<_>>().join(", "))
        }
        TypeKind::Fallible(t) => format!("{}!", named(t)),
        TypeKind::Pointer(t) => format!("*{}", named(t)),
        TypeKind::Slice(t) => format!("[]{}", named(t)),
        TypeKind::Array(t, n) => format!("[{n}]{}", named(t)),
        TypeKind::Map(k, v) => format!("map[{}]{}", named(k), named(v)),
        TypeKind::Func { params, ret } => format!(
            "func({}){}",
            params.iter().map(named).collect::<Vec<_>>().join(", "),
            ret.as_ref().map(|r| format!(" {}", named(r))).unwrap_or_default()
        ),
        TypeKind::Tuple(ts) => format!("({})", ts.iter().map(named).collect::<Vec<_>>().join(", ")),
    }
}

// ---------------------------------------------------------------------------
// DESIGN.md
// ---------------------------------------------------------------------------

/// All ```slang code blocks of DESIGN.md, with the section title they follow.
fn design_snippets() -> Vec<(String, String)> {
    let mut out = Vec::new();
    let mut section = String::new();
    let mut current: Option<String> = None;
    for line in DESIGN.lines() {
        if let Some(code) = current.as_mut() {
            if line.starts_with("```") {
                out.push((section.clone(), current.take().unwrap()));
            } else {
                code.push_str(line);
                code.push('\n');
            }
        } else if line.starts_with("```slang") {
            current = Some(String::new());
        } else if line.starts_with('#') {
            section = line.trim_start_matches('#').trim().to_string();
        }
    }
    out
}

/// Counts `{` minus `}` outside string and char literals and comments.
fn brace_delta(line: &str) -> i32 {
    let mut delta = 0;
    let mut chars = line.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '"' | '\'' => {
                while let Some(d) = chars.next() {
                    if d == '\\' {
                        chars.next();
                    } else if d == c {
                        break;
                    }
                }
            }
            '/' if chars.peek() == Some(&'/') => break,
            '{' => delta += 1,
            '}' => delta -= 1,
            _ => {}
        }
    }
    delta
}

/// Turns a snippet into a complete file: top-level declarations stay at
/// file level, loose statements go into a wrapper function, and a
/// namespace is added if missing. `...` placeholders become comments.
fn snippet_to_file(code: &str) -> String {
    let code = code.replace("...", "/* ... */");
    let decl_starts = ["namespace ", "import ", "struct ", "interface ", "enum ", "func ", "async func ", "@"];
    let (mut top, mut body) = (String::new(), String::new());
    let mut depth = 0;
    let mut in_decl = false;
    for line in code.lines() {
        if depth == 0 {
            in_decl = decl_starts.iter().any(|s| line.starts_with(s));
        }
        let target = if in_decl { &mut top } else { &mut body };
        target.push_str(line);
        target.push('\n');
        depth += brace_delta(line);
    }
    let ns = if top.trim_start().starts_with("namespace") { "" } else { "namespace snippet\n" };
    format!("{ns}{top}\nfunc __snippet() {{\n{body}}}\n")
}

#[test]
fn design_example_program() {
    let section = DESIGN.split("## Пример программы").nth(1).expect("example section");
    let src = section.split("```slang\n").nth(1).unwrap().split("```").next().unwrap();
    let file = parse_ok(src);

    assert_eq!(file.namespace.to_string(), "app");
    let imports: Vec<_> = file.imports.iter().map(|i| i.path.to_string()).collect();
    assert_eq!(imports, ["std", "std.io"]);
    assert_eq!(file.decls.len(), 4);

    let Decl::Struct(counter) = &file.decls[0] else { panic!() };
    assert_eq!(counter.name.name, "Counter");
    assert_eq!(counter.fields.len(), 1);
    assert_eq!(counter.fields[0].name.name, "count");
    assert_eq!(named(&counter.fields[0].ty), "int");
    let methods: Vec<_> = counter.methods.iter().map(|m| m.name.name.as_str()).collect();
    assert_eq!(methods, ["Inc", "Value"]);
    let inc = &counter.methods[0].body.as_ref().unwrap().stmts[0].kind;
    assert!(matches!(inc, StmtKind::Assign { op: AssignOp::Add, .. }), "{inc:?}");

    let Decl::Struct(parser_s) = &file.decls[1] else { panic!() };
    let parse_int = &parser_s.methods[0];
    assert!(parse_int.is_static && !parse_int.is_async);
    assert_eq!(named(parse_int.ret.as_ref().unwrap()), "int!");
    let StmtKind::Return(vals) = &parse_int.body.as_ref().unwrap().stmts[1].kind else { panic!() };
    assert_eq!(show(&vals[0]), "strings.ToInt(s)?");

    let Decl::Func(load) = &file.decls[2] else { panic!() };
    assert!(load.is_async);
    assert_eq!(named(load.ret.as_ref().unwrap()), "[]string!");
    let StmtKind::Var(text) = &load.body.as_ref().unwrap().stmts[0].kind else { panic!() };
    assert_eq!(show(text.init.as_ref().unwrap()), "(await io.ReadFileAsync(path))?");

    let Decl::Func(main) = &file.decls[3] else { panic!() };
    assert!(main.is_async && main.ret.is_none());
    let body = &main.body.as_ref().unwrap().stmts;
    assert_eq!(body.len(), 4);
    let StmtKind::Var(lines) = &body[0].kind else { panic!() };
    let ExprKind::Try { expr, handler: TryHandler::Catch { name, body: catch } } = &lines.init.as_ref().unwrap().kind
    else {
        panic!()
    };
    assert_eq!(show(expr), "(await Load(\"numbers.txt\"))");
    assert_eq!(name.name, "err");
    assert_eq!(catch.stmts.len(), 2);
    let StmtKind::Var(c) = &body[1].kind else { panic!() };
    assert_eq!(show(c.init.as_ref().unwrap()), "Counter{}");
    let StmtKind::For(f) = &body[2].kind else { panic!() };
    let ForKind::In { vars, iter } = &f.kind else { panic!() };
    assert_eq!((vars[0].name.as_str(), show(iter).as_str()), ("line", "lines"));
    let StmtKind::Var(n) = &f.body.stmts[0].kind else { panic!() };
    let ExprKind::Try { handler: TryHandler::Else(v), .. } = &n.init.as_ref().unwrap().kind else { panic!() };
    assert!(matches!(&**v, ElseValue::Expr(Expr { kind: ExprKind::Int(0), .. })));

    // Round trip on spans: every declaration's span covers exactly its source text.
    let sf = SourceFile::new("example.sl", src);
    let texts: Vec<_> = file.decls.iter().map(|d| sf.slice(d.span())).collect();
    assert!(texts[0].starts_with("struct Counter {") && texts[0].ends_with('}'));
    assert!(texts[1].starts_with("struct Parser {") && texts[1].ends_with('}'));
    assert!(texts[2].starts_with("async func Load(") && texts[2].ends_with('}'));
    assert!(texts[3].starts_with("async func main()") && texts[3].ends_with('}'));
    assert_eq!(sf.slice(file.span), src.trim_end());
    // Re-joining the declaration texts and parsing again gives the same shape.
    let rejoined = format!("namespace app\nimport std\nimport std.io\n{}\n", texts.join("\n\n"));
    let again = parse_ok(&rejoined);
    assert_eq!(again.decls.len(), file.decls.len());
    for (a, b) in again.decls.iter().zip(&file.decls) {
        assert_eq!(sf.slice(b.span()), &rejoined[a.span().start as usize..a.span().end as usize]);
    }
}

#[test]
fn all_design_snippets_parse() {
    let snippets = design_snippets();
    assert!(snippets.len() >= 20, "found only {} snippets", snippets.len());
    let mut failures = Vec::new();
    for (section, code) in &snippets {
        let src = snippet_to_file(code);
        let (file, diags) = try_parse(&src);
        if file.is_none() || diags.has_errors() {
            let sf = SourceFile::new(format!("DESIGN.md [{section}]"), src.clone());
            failures.push(format!("{}\n{src}", diags.render_all(&sf)));
        }
    }
    assert!(failures.is_empty(), "{} snippet(s) failed:\n{}", failures.len(), failures.join("\n=====\n"));
}

#[test]
fn design_ebnf_examples() {
    // Every construct of the grammar section, in one file.
    parse_ok(
        r#"namespace a.b.c
import x.y
import x.z as w
@reflect
struct S { F int; func M() {} ; static func N() S { return S{F: 1} } }
interface I { func M() }
enum E { A, B }
func f(a, b int, c string) (int, string)! {
    var x int = 1
    const K = 2
    if x > 0 { } else if x < 0 { } else { }
    for { break }
    for x < 10 { x += 1 }
    for i in 0..10 { continue }
    for i, v in list { defer g(i, v) }
    switch x { case 1, 2: g() case 3..4: g() case R.Ok(v): g(v) case T t: g(t) default: g() }
    return a, c
}
"#,
    );
}

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

#[test]
fn namespace_and_imports() {
    let f = parse_ok("namespace app.services.users\n\nimport std.io\nimport std.io as out\nimport app.models.User\n");
    assert_eq!(f.namespace.to_string(), "app.services.users");
    assert_eq!(f.imports.len(), 3);
    assert_eq!(f.imports[1].alias.as_ref().unwrap().name, "out");
    assert_eq!(f.imports[2].path.segments.len(), 3);
    assert!(f.decls.is_empty());
    // Keywords are allowed as path segments.
    let f = parse_ok("namespace examples.async\nimport std.default as def\nvar x a.map");
    assert_eq!(f.namespace.to_string(), "examples.async");
    assert_eq!(f.imports[0].path.to_string(), "std.default");
}

#[test]
fn example_corpus_parses() {
    // examples/ is owned by another agent; only read it here.
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../examples");
    let mut files = Vec::new();
    let mut stack = vec![dir];
    while let Some(d) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&d) else { continue };
        for e in entries.flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.extension().is_some_and(|x| x == "sl") {
                files.push(p);
            }
        }
    }
    let mut failures = Vec::new();
    for path in &files {
        let src = std::fs::read_to_string(path).unwrap();
        let (file, diags) = try_parse(&src);
        if file.is_none() || diags.has_errors() {
            let sf = SourceFile::new(path.display().to_string(), src.clone());
            failures.push(diags.render_all(&sf));
        }
    }
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}

#[test]
fn struct_members() {
    let d = decls(
        "struct Point {
            Base
            *Other
            pkg.Mixin
            X, Y float
            name string

            func Length() float { return 0.0 }
            static func New(x, y float) Point { return Point{X: x, Y: y} }
            static async func Load() Point! { return Point{} }
        }",
    );
    let Decl::Struct(s) = &d[0] else { panic!() };
    let fields: Vec<_> = s.fields.iter().map(|f| (f.name.name.as_str(), named(&f.ty))).collect();
    assert_eq!(fields, [("X", "float".into()), ("Y", "float".into()), ("name", "string".into())]);
    let embeds: Vec<_> = s.embeds.iter().map(named).collect();
    assert_eq!(embeds, ["Base", "*Other", "pkg.Mixin"]);
    assert_eq!(s.methods.len(), 3);
    assert!(!s.methods[0].is_static);
    assert!(s.methods[1].is_static);
    assert_eq!(s.methods[1].params.len(), 2);
    assert!(s.methods[2].is_static && s.methods[2].is_async);
}

#[test]
fn interface_and_enums() {
    let d = decls(
        "interface Shape {
            func Area() float
            func Scale(k float)
        }
        enum Color { Red, Green, Blue }
        enum Result {
            Ok(value int)
            Fail(code int, msg string)
        }
        enum Trailing {
            A,
            B,
        }",
    );
    let Decl::Interface(i) = &d[0] else { panic!() };
    assert_eq!(i.methods.len(), 2);
    assert!(i.methods.iter().all(|m| m.body.is_none()));
    assert_eq!(named(i.methods[0].ret.as_ref().unwrap()), "float");
    assert!(i.methods[1].ret.is_none());

    let Decl::Enum(c) = &d[1] else { panic!() };
    let names: Vec<_> = c.variants.iter().map(|v| v.name.name.as_str()).collect();
    assert_eq!(names, ["Red", "Green", "Blue"]);
    assert!(c.variants.iter().all(|v| v.fields.is_empty()));

    let Decl::Enum(r) = &d[2] else { panic!() };
    assert_eq!(r.variants[0].fields[0].name.name, "value");
    assert_eq!(r.variants[1].fields.len(), 2);
    let Decl::Enum(t) = &d[3] else { panic!() };
    assert_eq!(t.variants.len(), 2);
}

#[test]
fn functions_and_params() {
    let d = decls(
        "func Add(a int, b int) int { return a + b }
        func Add2(a, b int, s string) int { return a + b }
        func Swap(a, b int) (int, int) { return b, a }
        func Log(msg string) { println(msg) }
        async func Fetch(url string) string! { return \"\" }
        func Each(xs []int, f func(int) bool, _ int) {}",
    );
    let Decl::Func(add2) = &d[1] else { panic!() };
    let params: Vec<_> = add2.params.iter().map(|p| (p.name.name.as_str(), named(&p.ty))).collect();
    assert_eq!(params, [("a", "int".into()), ("b", "int".into()), ("s", "string".into())]);
    let Decl::Func(swap) = &d[2] else { panic!() };
    assert_eq!(named(swap.ret.as_ref().unwrap()), "(int, int)");
    let StmtKind::Return(v) = &swap.body.as_ref().unwrap().stmts[0].kind else { panic!() };
    assert_eq!(v.len(), 2);
    let Decl::Func(log) = &d[3] else { panic!() };
    assert!(log.ret.is_none());
    let Decl::Func(fetch) = &d[4] else { panic!() };
    assert!(fetch.is_async);
    let Decl::Func(each) = &d[5] else { panic!() };
    assert_eq!(named(&each.params[1].ty), "func(int) bool");
    assert_eq!(each.params[2].name.name, "_");
}

#[test]
fn top_level_var_and_const() {
    let d = decls("var Version = \"1.0\"\nconst Ratio = 2.5\nconst Max int = 10\nvar count int");
    let Decl::Var(v) = &d[0] else { panic!() };
    assert_eq!(v.names[0].name, "Version");
    let Decl::Const(c) = &d[1] else { panic!() };
    assert!(matches!(c.value.kind, ExprKind::Float(f) if f == 2.5));
    let Decl::Const(c) = &d[2] else { panic!() };
    assert_eq!(named(c.ty.as_ref().unwrap()), "int");
    let Decl::Var(v) = &d[3] else { panic!() };
    assert!(v.init.is_none());
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

#[test]
fn type_syntax() {
    assert_eq!(named(&ty("int")), "int");
    assert_eq!(named(&ty("app.models.User")), "app.models.User");
    assert_eq!(named(&ty("int!")), "int!");
    assert_eq!(named(&ty("void!")), "void!");
    assert_eq!(named(&ty("*Point")), "*Point");
    assert_eq!(named(&ty("[]User!")), "[]User!");
    assert!(matches!(ty("[]User!").kind, TypeKind::Fallible(_)), "`!` applies to the whole slice");
    assert_eq!(named(&ty("[4]byte")), "[4]byte");
    assert_eq!(named(&ty("[0x10]int")), "[16]int");
    assert_eq!(named(&ty("map[string][]int")), "map[string][]int");
    assert_eq!(named(&ty("func(int, string) bool")), "func(int, string) bool");
    assert_eq!(named(&ty("func()")), "func()");
    assert_eq!(named(&ty("func() int!")), "func() int!");
    assert_eq!(named(&ty("(int, string)")), "(int, string)");
    assert_eq!(named(&ty("(int)")), "int");
    assert_eq!(named(&ty("List<int>")), "List<int>");
    assert_eq!(named(&ty("Map<string, List<int>>")), "Map<string, List<int>>");
    assert_eq!(named(&ty("Task<List<List<int>>>")), "Task<List<List<int>>>");
    assert_eq!(named(&ty("Task<string!>")), "Task<string!>");
    assert_eq!(named(&ty("**[]map[K]*V")), "**[]map[K]*V");
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

#[test]
fn var_forms() {
    let s = stmts(
        "var x = 123
        var list []int = []
        var count int
        var (a, b) = Swap(1, 2)
        var (_, rest) = f()
        var (p, q) (int, int) = g()",
    );
    let vars: Vec<&VarDecl> = s.iter().map(|s| match &s.kind { StmtKind::Var(v) => v, _ => panic!() }).collect();
    assert!(vars[0].ty.is_none() && vars[0].init.is_some());
    assert_eq!(named(vars[1].ty.as_ref().unwrap()), "[]int");
    assert!(matches!(&vars[1].init.as_ref().unwrap().kind, ExprKind::SliceLit(v) if v.is_empty()));
    assert!(vars[2].init.is_none());
    let names: Vec<_> = vars[3].names.iter().map(|n| n.name.as_str()).collect();
    assert_eq!(names, ["a", "b"]);
    assert_eq!(vars[4].names[0].name, "_");
    assert_eq!(named(vars[5].ty.as_ref().unwrap()), "(int, int)");
}

#[test]
fn if_else_chain() {
    let StmtKind::If(i) = stmt("if x > 0 {\n a()\n} else if x < 0 {\n b()\n} else {\n c()\n}") else { panic!() };
    assert_eq!(show(&i.cond), "(x Gt 0)");
    let Some(ElseBranch::If(i2)) = i.else_.as_deref() else { panic!() };
    assert_eq!(show(&i2.cond), "(x Lt 0)");
    assert!(matches!(i2.else_.as_deref(), Some(ElseBranch::Block(b)) if b.stmts.len() == 1));
    // `else` on the next line is accepted too.
    let StmtKind::If(i) = stmt("if a {\n}\nelse {\n}") else { panic!() };
    assert!(i.else_.is_some());
}

#[test]
fn for_forms() {
    let s = stmts(
        "for { }
        for cond { }
        for x < 10 { }
        for item in list { }
        for i, item in list { }
        for i in 0..10 { }
        for _, v in m { }
        for i in 0..len(xs) - 1 { }",
    );
    let kinds: Vec<&ForKind> = s.iter().map(|s| match &s.kind { StmtKind::For(f) => &f.kind, _ => panic!() }).collect();
    assert!(matches!(kinds[0], ForKind::Infinite));
    assert!(matches!(kinds[1], ForKind::While(e) if show(e) == "cond"));
    assert!(matches!(kinds[2], ForKind::While(e) if show(e) == "(x Lt 10)"));
    assert!(matches!(kinds[3], ForKind::In { vars, iter } if vars.len() == 1 && show(iter) == "list"));
    assert!(matches!(kinds[4], ForKind::In { vars, .. } if vars[0].name == "i" && vars[1].name == "item"));
    assert!(matches!(kinds[5], ForKind::In { iter, .. } if show(iter) == "0..10"));
    assert!(matches!(kinds[6], ForKind::In { vars, .. } if vars[0].name == "_"));
    assert!(matches!(kinds[7], ForKind::In { iter, .. } if show(iter) == "0..(len(xs) Sub 1)"));
}

#[test]
fn switch_patterns() {
    let StmtKind::Switch(s) = stmt(
        "switch n {
            case 1, 2:       println(\"small\")
            case 3..10:      println(\"medium\")
            case n > 100:    println(\"huge\")
            case Result.Ok(v):
                println(v)
                println(v)
            case Circle c, *Rect r:
            case Point{X: 1}: f()
            default:         println(\"other\")
        }",
    ) else {
        panic!()
    };
    assert_eq!(show(s.subject.as_ref().unwrap()), "n");
    assert_eq!(s.cases.len(), 6);
    assert_eq!(s.cases[0].patterns.len(), 2);
    assert!(matches!(&s.cases[1].patterns[0].kind,
        PatternKind::Range(Some(a), Some(b)) if show(a) == "3" && show(b) == "10"));
    assert!(matches!(&s.cases[2].patterns[0].kind, PatternKind::Expr(e) if show(e) == "(n Gt 100)"));
    assert!(matches!(&s.cases[3].patterns[0].kind, PatternKind::Expr(e) if show(e) == "Result.Ok(v)"));
    assert_eq!(s.cases[3].body.len(), 2);
    let PatternKind::TypeBind { ty, name } = &s.cases[4].patterns[0].kind else { panic!() };
    assert_eq!((named(ty).as_str(), name.name.as_str()), ("Circle", "c"));
    let PatternKind::TypeBind { ty, name } = &s.cases[4].patterns[1].kind else { panic!() };
    assert_eq!((named(ty).as_str(), name.name.as_str()), ("*Rect", "r"));
    assert!(s.cases[4].body.is_empty());
    assert!(matches!(&s.cases[5].patterns[0].kind, PatternKind::Expr(e) if show(e) == "Point{X: 1}"));
    assert_eq!(s.default.as_ref().unwrap().len(), 1);
}

#[test]
fn ranges_and_slicing() {
    assert_eq!(show(&expr("s[1..3]")), "s[1..3]");
    assert_eq!(show(&expr("s[..2]")), "s[..2]");
    assert_eq!(show(&expr("s[2..]")), "s[2..]");
    assert_eq!(show(&expr("s[..]")), "s[..]");
    assert_eq!(show(&expr("s[i + 1..len(s) - 1]")), "s[(i Add 1)..(len(s) Sub 1)]");
    let StmtKind::For(f) = stmt("for i in 0.. {\n break\n}") else { panic!() };
    assert!(matches!(&f.kind, ForKind::In { iter, .. } if show(iter) == "0.."));
    let StmtKind::Switch(s) = stmt("switch n {\n case ..0: a()\n case 100..: b()\n case 1..9, 10: c()\n}") else { panic!() };
    assert!(matches!(&s.cases[0].patterns[0].kind, PatternKind::Range(None, Some(_))));
    assert!(matches!(&s.cases[1].patterns[0].kind, PatternKind::Range(Some(_), None)));
    assert!(matches!(&s.cases[2].patterns[1].kind, PatternKind::Expr(_)));
}

#[test]
fn switch_without_subject() {
    let StmtKind::Switch(s) = stmt(
        "switch {
            case errors.Is(err, net.ErrTimeout): retry()
            case a < b: return err
            default: panic(err)
        }",
    ) else {
        panic!()
    };
    assert!(s.subject.is_none());
    assert!(matches!(&s.cases[1].patterns[0].kind, PatternKind::Expr(e) if show(e) == "(a Lt b)"));
    assert!(matches!(s.cases[1].body[0].kind, StmtKind::Return(_)));
}

#[test]
fn simple_statements() {
    let s = stmts(
        "return
        return 1
        return a, b
        break
        continue
        defer f.Close()
        x = 1
        this.X += dx
        a.b[i] -= 2
        *p = 3
        x *= 2; x /= 2; x %= 2; x &= 1; x |= 1; x ^= 1; x <<= 1; x >>= 1
        a, b = b, a
        _ = f()
        f()
        {
            nested()
        }",
    );
    assert!(matches!(&s[0].kind, StmtKind::Return(v) if v.is_empty()));
    assert!(matches!(&s[1].kind, StmtKind::Return(v) if v.len() == 1));
    assert!(matches!(&s[2].kind, StmtKind::Return(v) if v.len() == 2));
    assert!(matches!(s[3].kind, StmtKind::Break));
    assert!(matches!(s[4].kind, StmtKind::Continue));
    assert!(matches!(&s[5].kind, StmtKind::Defer(e) if show(e) == "f.Close()"));
    assert!(matches!(&s[6].kind, StmtKind::Assign { op: AssignOp::Assign, .. }));
    assert!(matches!(&s[7].kind, StmtKind::Assign { op: AssignOp::Add, target, .. } if show(target) == "this.X"));
    assert!(matches!(&s[8].kind, StmtKind::Assign { op: AssignOp::Sub, target, .. } if show(target) == "a.b[i]"));
    assert!(matches!(&s[9].kind, StmtKind::Assign { target, .. } if show(target) == "(*p)"));
    let ops: Vec<AssignOp> = s[10..18]
        .iter()
        .map(|s| match s.kind {
            StmtKind::Assign { op, .. } => op,
            _ => panic!(),
        })
        .collect();
    use AssignOp::*;
    assert_eq!(ops, [Mul, Div, Rem, BitAnd, BitOr, BitXor, Shl, Shr]);
    assert!(matches!(&s[18].kind, StmtKind::Assign { target, value, .. }
        if show(target) == "tuple(a, b)" && show(value) == "tuple(b, a)"));
    assert!(matches!(&s[19].kind, StmtKind::Assign { target, .. } if show(target) == "_"));
    assert!(matches!(&s[20].kind, StmtKind::Expr(e) if show(e) == "f()"));
    assert!(matches!(&s[21].kind, StmtKind::Block(b) if b.stmts.len() == 1));
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

#[test]
fn literals() {
    assert!(matches!(expr("0xff").kind, ExprKind::Int(255)));
    assert!(matches!(expr("0b1010").kind, ExprKind::Int(10)));
    assert!(matches!(expr("0o17").kind, ExprKind::Int(15)));
    assert!(matches!(expr("1_000_000").kind, ExprKind::Int(1_000_000)));
    assert!(matches!(expr("18446744073709551615").kind, ExprKind::Int(u64::MAX)));
    assert!(matches!(expr("2.5e3").kind, ExprKind::Float(f) if f == 2500.0));
    assert!(matches!(expr("\"a\\n\"").kind, ExprKind::Str(ref s) if s == "a\n"));
    assert!(matches!(expr("'x'").kind, ExprKind::Char('x')));
    assert!(matches!(expr("true").kind, ExprKind::Bool(true)));
    assert!(matches!(expr("false").kind, ExprKind::Bool(false)));
    assert!(matches!(expr("nil").kind, ExprKind::Nil));
    assert!(matches!(expr("this").kind, ExprKind::This));
}

#[test]
fn precedence() {
    assert_eq!(show(&expr("1 + 2 * 3")), "(1 Add (2 Mul 3))");
    assert_eq!(show(&expr("1 - 2 - 3")), "((1 Sub 2) Sub 3)");
    assert_eq!(show(&expr("a || b && c == d")), "(a Or (b And (c Eq d)))");
    assert_eq!(show(&expr("a + b < c * d")), "((a Add b) Lt (c Mul d))");
    assert_eq!(show(&expr("a & b | c ^ d << 1")), "(((a BitAnd b) BitOr c) BitXor (d Shl 1))");
    assert_eq!(show(&expr("-a * !b")), "((-a) Mul (!b))");
    assert_eq!(show(&expr("&p.X")), "(&p.X)");
    assert_eq!(show(&expr("*ptr + 1")), "((*ptr) Add 1)");
    assert_eq!(show(&expr("(1 + 2) * 3")), "([(1 Add 2)] Mul 3)");
    assert_eq!(show(&expr("a.b.c(1)[2].d")), "a.b.c(1)[2].d");
    assert_eq!(show(&expr("a >= b != (c <= d)")), "((a Ge b) Ne [(c Le d)])");
    assert_eq!(show(&expr("x % 2 / 3 >> 1")), "(((x Rem 2) Div 3) Shr 1)");
}

#[test]
fn error_operators() {
    assert_eq!(show(&expr("io.ReadFile(path)?")), "io.ReadFile(path)?");
    assert_eq!(show(&expr("io.ReadFile(path)?(\"reading config\")")), "io.ReadFile(path)?(\"reading config\")");
    assert_eq!(show(&expr("f()?.g()?")), "f()?.g()?");
    assert_eq!(show(&expr("-f()?")), "(-f()?)");
    assert_eq!(show(&expr("await f()")), "(await f())");
    assert_eq!(show(&expr("await http.Get(url)?")), "(await http.Get(url))?");
    assert_eq!(show(&expr("await task.All(a, b)?(\"ctx\")")), "(await task.All(a, b))?(\"ctx\")");

    let e = expr("try ParseInt(env.Get(\"PORT\")) else 8080");
    let ExprKind::Try { expr: inner, handler: TryHandler::Else(v) } = e.kind else { panic!() };
    assert_eq!(show(&inner), "ParseInt(env.Get(\"PORT\"))");
    assert!(matches!(*v, ElseValue::Expr(Expr { kind: ExprKind::Int(8080), .. })));

    let s = stmts(
        "var a = try f() else continue
        var b = try f() else break
        var c = try f() else return
        var d = try f() else return 1, err
        try Save(\"out.txt\", data) catch err {
            println(\"save failed:\", err)
        }
        var e = try f()
        catch _ { return }",
    );
    let handler = |i: usize| -> TryHandler {
        let e = match &s[i].kind {
            StmtKind::Var(v) => v.init.clone().unwrap(),
            StmtKind::Expr(e) => e.clone(),
            other => panic!("{other:?}"),
        };
        match e.kind {
            ExprKind::Try { handler, .. } => handler,
            other => panic!("{other:?}"),
        }
    };
    assert!(matches!(handler(0), TryHandler::Else(v) if matches!(*v, ElseValue::Continue(_))));
    assert!(matches!(handler(1), TryHandler::Else(v) if matches!(*v, ElseValue::Break(_))));
    assert!(matches!(handler(2), TryHandler::Else(v) if matches!(&*v, ElseValue::Return(r, _) if r.is_empty())));
    assert!(matches!(handler(3), TryHandler::Else(v) if matches!(&*v, ElseValue::Return(r, _) if r.len() == 2)));
    assert!(matches!(handler(4), TryHandler::Catch { name, body } if name.name == "err" && body.stmts.len() == 1));
    assert!(matches!(handler(5), TryHandler::Catch { name, .. } if name.name == "_"));
}

#[test]
fn composite_literals() {
    assert_eq!(show(&expr("Point{X: 1, Y: 2}")), "Point{X: 1, Y: 2}");
    assert_eq!(show(&expr("Point{}")), "Point{}");
    assert_eq!(show(&expr("app.models.User{Name: \"a\"}")), "app.models.User{Name: \"a\"}");
    assert_eq!(show(&expr("Point{\n X: 1,\n Y: 2,\n}")), "Point{X: 1, Y: 2}");
    assert_eq!(show(&expr("Point{\n X: 1,\n Y: 2\n}")), "Point{X: 1, Y: 2}");
    assert_eq!(show(&expr("new Point{X: 1}")), "new Point{X: 1}");
    assert_eq!(show(&expr("new int")), "new int");
    assert_eq!(show(&expr("new app.Point{}")), "new app.Point{}");
    assert_eq!(show(&expr("[1, 2, 3]")), "[1, 2, 3]");
    assert_eq!(show(&expr("[\n1,\n2\n]")), "[1, 2]");
    assert_eq!(show(&expr("map[string]int{\"a\": 1, \"b\": 2}")), "map{\"a\": 1, \"b\": 2}");
    assert_eq!(show(&expr("map[string]int{}")), "map{}");
    assert_eq!(show(&expr("(1, \"a\")")), "tuple(1, \"a\")");
    assert_eq!(show(&expr("Outer{In: Inner{V: [1]}}")), "Outer{In: Inner{V: [1]}}");
    // Lowercase struct names (attribute structs) work outside headers.
    assert_eq!(show(&expr("tag{name: \"x\"}")), "tag{name: \"x\"}");
    // In a header `p.x {` stays an expression.
    let StmtKind::If(i) = stmt("if p.x {\n}") else { panic!() };
    assert_eq!(show(&i.cond), "p.x");
    // `new` accepts any primary expression.
    assert_eq!(show(&expr("new (Point{})")), "new [Point{}]");
    assert_eq!(show(&expr("new [1]")), "new [1]");
}

#[test]
fn struct_literal_ambiguity_in_headers() {
    // `{` after a name in an if/for/switch header opens the block.
    let StmtKind::If(i) = stmt("if x == Point {\n f()\n}") else { panic!() };
    assert_eq!(show(&i.cond), "(x Eq Point)");
    assert_eq!(i.then.stmts.len(), 1);
    // Parentheses re-enable struct literals.
    let StmtKind::If(i) = stmt("if x == (Point{}) {\n}") else { panic!() };
    assert_eq!(show(&i.cond), "(x Eq [Point{}])");
    // So do call arguments and brackets.
    let StmtKind::If(i) = stmt("if eq(x, Point{X: 1}) && ys[Key{}] {\n}") else { panic!() };
    assert_eq!(show(&i.cond), "(eq(x, Point{X: 1}) And ys[Key{}])");
    let StmtKind::For(f) = stmt("for p in Points {\n}") else { panic!() };
    assert!(matches!(&f.kind, ForKind::In { iter, .. } if show(iter) == "Points"));
    let StmtKind::For(f) = stmt("for Ready {\n}") else { panic!() };
    assert!(matches!(&f.kind, ForKind::While(e) if show(e) == "Ready"));
    let StmtKind::Switch(s) = stmt("switch Color {\n case Red: f()\n}") else { panic!() };
    assert_eq!(show(s.subject.as_ref().unwrap()), "Color");
    // Inside the block itself struct literals work again.
    let StmtKind::If(i) = stmt("if ok {\n var p = Point{X: 1}\n}") else { panic!() };
    assert!(matches!(&i.then.stmts[0].kind, StmtKind::Var(v) if show(v.init.as_ref().unwrap()) == "Point{X: 1}"));
}

#[test]
fn lambdas_and_parens() {
    let e = expr("func(x int) int { return x * 2 }");
    let ExprKind::Lambda { params, ret, body } = e.kind else { panic!() };
    assert_eq!(params.len(), 1);
    assert_eq!(named(ret.as_deref().unwrap()), "int");
    assert!(matches!(*body, LambdaBody::Block(_)));

    let e = expr("(x int) => x * 2");
    let ExprKind::Lambda { params, ret, body } = e.kind else { panic!() };
    assert_eq!(params[0].name.name, "x");
    assert!(ret.is_none());
    assert!(matches!(*body, LambdaBody::Expr(ref e) if show(e) == "(x Mul 2)"));

    let ExprKind::Lambda { params, .. } = expr("(a, b int) => a + b").kind else { panic!() };
    assert_eq!(params.len(), 2);
    assert!(matches!(expr("() => 1").kind, ExprKind::Lambda { ref params, .. } if params.is_empty()));
    assert!(matches!(expr("(xs []int) => { return len(xs) }").kind,
        ExprKind::Lambda { ref body, .. } if matches!(**body, LambdaBody::Block(_))));
    assert!(matches!(expr("(f func(int) int, m map[string]int) => f(m[\"a\"])").kind, ExprKind::Lambda { .. }));
    assert_eq!(show(&expr("(a, b)")), "tuple(a, b)");
    assert_eq!(show(&expr("(a)")), "[a]");
    assert_eq!(show(&expr("apply((x int) => x, 1)")), "apply(lambda/1, 1)");
    assert_eq!(show(&expr("func() {}")), "lambda/0");
}

// ---------------------------------------------------------------------------
// Attributes and generics
// ---------------------------------------------------------------------------

#[test]
fn attributes() {
    let f = parse_ok(
        r#"@doc("models")
namespace app.models

@json(name: "user_id")
@reflect
struct User {
    @json(name: "id")       Id    int
    @json(omitEmpty: true)  Email string
    @range(1, 64)           Name, Alias string
    plain int

    @deprecated("use Save")
    func Store() void! { return }
    @inline static func New() User { return User{} }
}

enum Color {
    @json(name: "r") Red
    @default
    Green,
    Blue
}

@test
func TestX(@json("v") value int, @a x, y string) {}

@attribute(targets: [reflect.Target.Field], repeatable: true)
struct range {
    min int
    max int
    message string = "value out of range"
}

interface I {
    @deprecated func M()
}

@export const K = 1
@export var V = 2
"#,
    );
    assert_eq!(f.attrs[0].path.to_string(), "doc");

    let Decl::Struct(user) = &f.decls[0] else { panic!() };
    let names: Vec<_> = user.attrs.iter().map(|a| a.path.to_string()).collect();
    assert_eq!(names, ["json", "reflect"]);
    let arg = &user.attrs[0].args[0];
    assert_eq!(arg.name.as_ref().unwrap().name, "name");
    assert!(matches!(&arg.value.kind, ExprKind::Str(s) if s == "user_id"));
    assert!(user.attrs[1].args.is_empty());
    assert!(f.decls[0].span().start < user.name.span.start, "decl span includes attributes");

    assert_eq!(user.fields.len(), 5);
    assert_eq!(user.fields[0].attrs[0].path.to_string(), "json");
    assert_eq!(user.fields[0].name.name, "Id");
    assert!(matches!(&user.fields[1].attrs[0].args[0].value.kind, ExprKind::Bool(true)));
    // Positional arguments; grouped fields share the attributes.
    let positional = &user.fields[2].attrs[0].args;
    assert!(positional.iter().all(|a| a.name.is_none()) && positional.len() == 2);
    assert_eq!(user.fields[3].name.name, "Alias");
    assert_eq!(user.fields[3].attrs.len(), 1);
    assert!(user.fields[4].attrs.is_empty());

    assert_eq!(user.methods[0].attrs[0].path.to_string(), "deprecated");
    assert_eq!(user.methods[1].attrs[0].path.to_string(), "inline");
    assert!(user.methods[1].is_static);

    let Decl::Enum(color) = &f.decls[1] else { panic!() };
    assert_eq!(color.variants.len(), 3);
    assert_eq!(color.variants[0].attrs[0].path.to_string(), "json");
    assert_eq!(color.variants[1].attrs[0].path.to_string(), "default");
    assert!(color.variants[2].attrs.is_empty());

    let Decl::Func(test) = &f.decls[2] else { panic!() };
    assert_eq!(test.attrs[0].path.to_string(), "test");
    assert_eq!(test.params[0].attrs[0].path.to_string(), "json");
    assert_eq!(test.params[1].attrs[0].path.to_string(), "a");
    assert!(test.params[2].attrs.is_empty());

    let Decl::Struct(range) = &f.decls[3] else { panic!() };
    assert_eq!(range.attrs[0].args.len(), 2);
    assert!(matches!(&range.attrs[0].args[0].value.kind, ExprKind::SliceLit(v) if v.len() == 1));
    assert!(matches!(&range.fields[2].default, Some(Expr { kind: ExprKind::Str(_), .. })));
    assert!(range.fields[0].default.is_none());

    let Decl::Interface(i) = &f.decls[4] else { panic!() };
    assert_eq!(i.methods[0].attrs.len(), 1);
    let Decl::Const(k) = &f.decls[5] else { panic!() };
    assert_eq!(k.attrs.len(), 1);
    let Decl::Var(v) = &f.decls[6] else { panic!() };
    assert_eq!(v.attrs.len(), 1);
}

#[test]
fn generics() {
    let d = decls(
        "func ToJson<T>(value T) string { return \"\" }
        struct Box<T: Shape, U> { Value T }
        interface Container<T> { func Get() T }",
    );
    let Decl::Func(f) = &d[0] else { panic!() };
    assert_eq!(f.generics[0].name.name, "T");
    assert_eq!(named(&f.params[0].ty), "T");
    let Decl::Struct(s) = &d[1] else { panic!() };
    assert_eq!(s.generics.len(), 2);
    assert_eq!(named(s.generics[0].bound.as_ref().unwrap()), "Shape");
    let Decl::Interface(i) = &d[2] else { panic!() };
    assert_eq!(i.generics.len(), 1);

    assert_eq!(show(&expr("reflect.TypeOf<User>()")), "reflect.TypeOf<1>()");
    assert_eq!(show(&expr("v.Field(\"Email\").Get<string>()")), "v.Field(\"Email\").Get<1>()");
    assert_eq!(show(&expr("m.Call<func(*User) void!>()")), "m.Call<1>()");
    assert_eq!(show(&expr("Pair<int, List<int>>(1, 2)")), "Pair<2>(1, 2)");
    assert_eq!(show(&expr("List<int>{}")), "List<1>{}");
    assert_eq!(show(&expr("new List<int>{}")), "new List<1>{}");
    // Comparisons are unaffected.
    assert_eq!(show(&expr("a < b")), "(a Lt b)");
    assert_eq!(show(&expr("a < b && c > d")), "((a Lt b) And (c Gt d))");
    assert_eq!(show(&expr("a < b >> c")), "(a Lt (b Shr c))");
    assert_eq!(show(&expr("i < n")), "(i Lt n)");
    let StmtKind::For(f) = stmt("for i < N {\n}") else { panic!() };
    assert!(matches!(&f.kind, ForKind::While(e) if show(e) == "(i Lt N)"));
    let StmtKind::For(f) = stmt("for v in reflect.TypeOf<Color>().Variants {\n}") else { panic!() };
    assert!(matches!(&f.kind, ForKind::In { iter, .. } if show(iter) == "reflect.TypeOf<1>().Variants"));
}

// ---------------------------------------------------------------------------
// Errors and recovery
// ---------------------------------------------------------------------------

fn messages(src: &str) -> Vec<String> {
    parse_err(src).into_iter().map(|d| d.message).collect()
}

#[test]
fn error_messages_are_expected_found() {
    assert_eq!(messages("namespace t\nfunc f() {\n var = 1\n}"), ["expected identifier, found `=`"]);
    assert_eq!(messages("namespace t\nfunc f() {\n x = \n}"), ["expected expression, found `}`"]);
    assert_eq!(messages("namespace t\nfunc f() {\n x = f(;\n}"), ["expected expression, found end of line"]);
    assert_eq!(messages("namespace t\nfunc f( {}"), ["expected identifier, found `{`"]);
    assert_eq!(messages("namespace t\nfunc f() {\n if x { } else return\n}"), ["expected `{`, found `return`"]);
    assert_eq!(messages("namespace t\nstruct S { 1 }"), ["expected field, method or `}`, found number `1`"]);
    assert_eq!(messages("func main() {}"), ["expected `namespace`, found `func`"]);
    assert_eq!(messages("namespace t\nfunc f() { var x }"), ["expected type or `=`, found `}`"]);
    assert_eq!(messages("namespace t\nfunc f() { a b }"), ["expected end of line, found identifier `b`"]);
    assert_eq!(messages("namespace t\nfunc f() { try f() }"), ["expected `catch` or `else`, found `}`"]);
    assert_eq!(messages("namespace t\nfunc f(a, b) {}"), ["expected parameter type, found `)`"]);
    assert_eq!(messages("namespace t\nvar x = 99999999999999999999"), ["integer literal `99999999999999999999` is too large"]);
    assert_eq!(messages("namespace t\nstatic func f() {}"), ["`static` is only allowed on methods inside a struct"]);
    assert_eq!(
        messages("namespace t\nfunc f() { switch x { default: a() default: b() } }"),
        ["multiple `default` clauses in switch"]
    );
    assert_eq!(messages("namespace t\nfunc f() { switch x { x } }"), ["expected `case`, `default` or `}`, found identifier `x`"]);
    assert_eq!(messages("namespace t\ninterface I { func M() {} }"), ["interface methods cannot have a body"]);
    assert_eq!(messages("namespace t\n@json(name: ) struct S {}"), ["expected expression, found `)`"]);
}

#[test]
fn error_spans_point_at_the_token() {
    let src = "namespace t\nfunc f() {\n    var = 1\n}\n";
    let d = parse_err(src);
    let sf = SourceFile::new("t.sl", src);
    assert_eq!(sf.slice(d[0].span), "=");
    assert_eq!(sf.line_col(d[0].span.start), (3, 9));
    let rendered = d[0].render(&sf);
    assert!(rendered.contains("t.sl:3:9"), "{rendered}");
}

#[test]
fn recovers_and_reports_multiple_errors() {
    let src = "namespace t

func a() {
    var = 1
    var ok = 2
    x = = 3
    if { }
    return ok
}

struct S {
    X int
    1 2 3
    Y float
}

func b( {
}

func c() int { return 1 }
";
    let d = parse_err(src);
    assert!(d.len() >= 5, "expected several errors, got {d:#?}");
    assert!(d.iter().all(|d| d.message.starts_with("expected")), "{d:#?}");
    let sf = SourceFile::new("t.sl", src);
    let lines: Vec<usize> = d.iter().map(|d| sf.line_col(d.span.start).0).collect();
    for line in [4, 6, 7, 13, 17] {
        assert!(lines.contains(&line), "no error on line {line}: {lines:?}");
    }
}

#[test]
fn errors_inside_switch_cases_recover() {
    let d = parse_err(
        "namespace t
func f() {
    switch x {
        case 1 +: a()
        case 2: b( = 1
        case 3: ok()
        default: c()
    }
    after()
}",
    );
    assert_eq!(d.len(), 2, "{d:#?}");
}

#[test]
fn lexer_errors_flow_through_diagnostics() {
    let (_, diags) = try_parse("namespace t\nvar s = \"unterminated\nvar c = 'ab'\n");
    let msgs: Vec<_> = diags.items.iter().map(|d| d.message.as_str()).collect();
    assert!(msgs.contains(&"unterminated string literal"), "{msgs:?}");
    assert!(msgs.contains(&"char literal must contain exactly one character"), "{msgs:?}");
}

#[test]
fn never_panics_on_truncated_input() {
    // Every prefix of a realistic program must produce a result, not a panic.
    let section = DESIGN.split("## Пример программы").nth(1).unwrap();
    let src = section.split("```slang\n").nth(1).unwrap().split("```").next().unwrap();
    let extra = "\nfunc g<T>(x map[string]List<List<int>>) { var f = (a, b int) => a + b; switch { case T t, x > 1: break }; var p = new Point{X: [1, 2], Y: map[int]int{1: 2}}?(\"c\"); for i, v in 0..10 { defer h() } }\n@a(b: 1) enum E { @x A(v int), B }\n";
    let full = format!("{src}{extra}");
    let mut boundaries: Vec<usize> = full.char_indices().map(|(i, _)| i).collect();
    boundaries.push(full.len());
    for &end in &boundaries {
        let _ = try_parse(&full[..end]);
    }
}

#[test]
fn never_panics_on_garbage() {
    let inputs = [
        "",
        "namespace",
        "namespace .",
        "}}}}",
        "namespace t\n}}}} func",
        "namespace t\nfunc f() { ((((( }",
        "namespace t\nfunc f() { [[[[ }",
        "namespace t\nfunc f() { case default: }",
        "namespace t\nstruct { } enum { } interface { }",
        "namespace t\nfunc f() { var (a, = 1; var x List<List<int> = 1; a < b >> c }",
        "namespace t\nfunc f() { switch { case : } }",
        "namespace t\n@ @ @",
        "namespace t\nfunc f() { x?(; try; await; new; map[; func( }",
        "namespace t\nfunc f() { (x int) => ; () => }",
        "namespace t\nvar x = [1, 2,, 3]",
        "namespace t\nfunc f<>() {}",
        ") ] } ; , . .. : ? ! => & * + - / % | ^ << >> && || < > <= >= == != = += @",
    ];
    for src in inputs {
        let (file, diags) = try_parse(src);
        assert!(file.is_none() == diags.has_errors(), "{src}");
    }
    // Every token kind in every position of a short window.
    let toks = ["(", ")", "{", "}", "[", "]", ",", ".", "..", ":", "\n", "?", "!", "=>", "@", "&", "*", "-", "<", ">", ">>",
        "=", "+=", "x", "X", "1", "\"s\"", "if", "for", "switch", "case", "default", "try", "catch", "else", "func", "var",
        "struct", "return", "new", "map", "await", "in", "_"];
    for a in toks {
        for b in toks {
            for c in ["(", "{", "x", "}", "\n", "<"] {
                let src = format!("namespace t\nfunc f() {{ {a} {b} {c} }}\nstruct S {{ {a} {b} }}\n");
                let _ = try_parse(&src);
            }
        }
    }
}

#[test]
fn deep_nesting_is_an_error_not_a_crash() {
    let deep = format!("namespace t\nvar x = {}1{}", "(".repeat(5000), ")".repeat(5000));
    let msgs = messages(&deep);
    assert!(msgs.iter().any(|m| m == "nesting is too deep"), "{msgs:?}");
    let deep = format!("namespace t\nfunc f() {}{}", "{".repeat(5000), "}".repeat(5000));
    assert!(messages(&deep).iter().any(|m| m == "nesting is too deep"));
    let deep = format!("namespace t\nvar x {}int", "[]".repeat(5000));
    assert!(messages(&deep).iter().any(|m| m == "nesting is too deep"));
    let deep = format!("namespace t\nvar x = {}1", "-".repeat(5000));
    assert!(messages(&deep).iter().any(|m| m == "nesting is too deep"));
}

#[test]
fn successful_parse_has_no_diagnostics() {
    let (file, diags) = try_parse("namespace t\nfunc main() {\n    println(\"hi\")\n}\n");
    assert!(file.is_some());
    assert!(diags.is_empty());
}
