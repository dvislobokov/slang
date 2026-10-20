//! Generics: inference, constraints, monomorphisation (checked on the
//! HIR), nested and recursive instantiations, generic enums and the
//! `std.collections` types. End-to-end behaviour is covered by
//! `tests/spec/generics`.

use slang::diagnostics::{Diagnostics, Severity};
use slang::driver;
use slang::hir::*;
use slang::span::SourceFile;
use slang::types::Type;

fn analyze(src: &str) -> (Option<Program>, Vec<String>, Vec<String>) {
    let files = vec![SourceFile::new("g.sl".to_string(), src.to_string())];
    let mut diags = Diagnostics::new();
    let p = driver::check(&files, &mut diags);
    let errors: Vec<_> = diags.items.iter().filter(|d| d.severity == Severity::Error).collect();
    let msgs = errors.iter().map(|d| d.message.clone()).collect();
    let notes = errors.iter().flat_map(|d| d.notes.iter().map(|n| n.0.clone())).collect();
    (p, msgs, notes)
}

/// A program: namespace `t`, `import std.io`, then `body`.
fn prog(body: &str) -> String {
    format!("namespace t\nimport std.io\n{body}\n")
}

fn ok(body: &str) -> Program {
    let (p, errors, _) = analyze(&prog(body));
    assert!(errors.is_empty(), "unexpected errors: {errors:#?}");
    p.expect("program")
}

fn err(body: &str, needle: &str) -> Vec<String> {
    let (p, errors, notes) = analyze(&prog(body));
    assert!(p.is_none(), "expected failure");
    assert!(errors.iter().any(|e| e.contains(needle)), "no error containing {needle:?} in {errors:#?}");
    notes
}

fn funcs_named<'p>(p: &'p Program, name: &str) -> Vec<&'p Function> {
    p.funcs.iter().filter(|f| f.name == name).collect()
}

fn no_params_left(p: &Program) {
    for f in &p.funcs {
        for l in &f.locals {
            assert!(!matches!(p.types.get(l.ty), Type::Param(_)), "type parameter left in {}: {}", f.full_name, l.name);
        }
    }
}

const MAX: &str = "func Max<T: Ordered>(a, b T) T {\n    if a > b {\n        return a\n    }\n    return b\n}\n";

#[test]
fn inference_from_arguments() {
    let p = ok(&format!("{MAX}func main() {{\n    var f float = 1\n    println(Max(1, 2), Max(f, 2), Max(\"a\", \"b\"), Max<int8>(1, 2))\n}}"));
    // Untyped constants take the type inferred from the other argument.
    for name in ["Max<int>", "Max<float>", "Max<string>", "Max<int8>"] {
        let f = funcs_named(&p, name);
        assert_eq!(f.len(), 1, "{name}");
        assert_eq!(f[0].symbol, format!("slang_t__{}", name.replace('<', "__").replace('>', "")));
    }
    no_params_left(&p);
}

#[test]
fn inference_through_slices_funcs_and_structs() {
    let p = ok("struct Box<T> {\n    V T\n}\nfunc Unbox<T>(b Box<T>) T {\n    return b.V\n}\nfunc Apply<T, U>(xs []T, f func(T) U) []U {\n    var out []U = []\n    for x in xs {\n        out = append(out, f(x))\n    }\n    return out\n}\nfunc main() {\n    println(Unbox(Box<string>{V: \"s\"}), Apply([1, 2], (x int) => x > 1))\n}");
    assert_eq!(funcs_named(&p, "Unbox<string>").len(), 1);
    assert_eq!(funcs_named(&p, "Apply<int, bool>").len(), 1);
}

#[test]
fn inference_errors() {
    err("func Id<T>(x T) T {\n    return x\n}\nfunc main() {\n    var x = Id(nil)\n    _ = x\n}", "cannot infer type parameter T of Id");
    err("func Two<T>(a, b T) {\n}\nfunc main() {\n    Two(1, \"s\")\n}", "conflicting types for type parameter T of Two");
    err("func Two<T>(a []T, b []T) {\n}\nfunc main() {\n    Two([1], [\"s\"])\n}", "conflicting types for type parameter T of Two: int and string");
    err("func Id<T>(x T) T {\n    return x\n}\nfunc main() {\n    println(Id<int, int>(1))\n}", "wrong number of type arguments for Id: expected 1, got 2");
    err("struct S<A, B> {\n    X A\n}\nfunc main() {\n    var s S<int>\n    _ = s\n}", "wrong number of type arguments for S");
    err("func Id<T>(x T) T {\n    return x\n}\nfunc main() {\n    var f = Id\n    _ = f\n}", "needs type arguments");
    err("struct S<T> {\n    X T\n}\nfunc main() {\n    var s = S{X: 1}\n    _ = s\n}", "missing type arguments for generic type S");
    err("struct P {\n    X int\n}\nfunc main() {\n    var p = P<int>{X: 1}\n    _ = p\n}", "is not generic");
}

#[test]
fn constraints_are_checked_at_instantiation() {
    err(&format!("struct P {{\n    X int\n}}\n{MAX}func main() {{\n    var m = Max(P{{X: 1}}, P{{X: 2}})\n    _ = m\n}}"), "type P does not satisfy constraint Ordered");
    err(&format!("{MAX}func main() {{\n    println(Max(true, false))\n}}"), "does not satisfy constraint Ordered");
    let shape = "interface Shape {\n    func Area() float\n}\nstruct Sq {\n    S float\n}\nfunc Total<T: Shape>(xs []T) float {\n    var t = 0.0\n    for x in xs {\n        t += x.Area()\n    }\n    return t\n}\n";
    err(&format!("{shape}func main() {{\n    println(Total([Sq{{S: 1}}]))\n}}"), "type Sq does not satisfy constraint Shape: missing method Area");
    err("func Eq<T: Comparable>(a, b T) bool {\n    return a == b\n}\nfunc main() {\n    println(Eq([1], [2]))\n}", "does not satisfy constraint Comparable");
}

#[test]
fn bodies_are_checked_against_constraints() {
    err("func Add<T>(a, b T) T {\n    return a + b\n}\nfunc main() {\n}", "operator + is not defined on type parameter T");
    err("func Eq<T>(a, b T) bool {\n    return a == b\n}\nfunc main() {\n}", "operator == is not defined on type parameter T");
    err("func Sub<T: Ordered>(a, b T) T {\n    return a - b\n}\nfunc main() {\n}", "operator - is not defined on type parameter T");
    err("interface Shape {\n    func Area() float\n}\nfunc F<T: Shape>(x T) {\n    x.Perimeter()\n}\nfunc main() {\n}", "type parameter T has no method Perimeter");
    err("struct S<T> {\n    V T\n    func Get() T {\n        return this.V.X\n    }\n}\nfunc main() {\n}", "has no field X");
    // Reported once, even with several instantiations.
    let (_, errors, _) = analyze(&prog("func Add<T>(a, b T) T {\n    return a + b\n}\nfunc main() {\n    println(Add(1, 2), Add(\"a\", \"b\"))\n}"));
    assert_eq!(errors.iter().filter(|e| e.contains("operator +")).count(), 1, "{errors:#?}");
    // Comparable allows ==, an interface constraint allows its methods.
    ok("func Eq<T: Comparable>(a, b T) bool {\n    return a == b\n}\nfunc main() {\n    println(Eq(1, 2), Eq(\"a\", \"a\"))\n}");
}

#[test]
fn errors_in_instances_name_the_instantiation() {
    let notes = err("struct P {\n    X int\n}\nfunc Show<T>(x T) {\n    println(x)\n}\nfunc main() {\n    Show(P{X: 1})\n}", "cannot be printed");
    assert!(notes.iter().any(|n| n == "in instantiation of Show<P>"), "{notes:#?}");
}

#[test]
fn each_instantiation_is_compiled_once() {
    let p = ok(&format!("{MAX}struct Stack<T> {{\n    items []T\n    func Push(x T) {{\n        this.items = append(this.items, x)\n    }}\n}}\nfunc main() {{\n    println(Max(1, 2), Max(3, 4), Max<int>(5, 6))\n    var a = Stack<int>{{}}\n    var b Stack<int>\n    a.Push(1)\n    b.Push(2)\n    var c = Stack<string>{{}}\n    c.Push(\"x\")\n}}"));
    assert_eq!(funcs_named(&p, "Max<int>").len(), 1);
    assert_eq!(p.structs.iter().filter(|s| s.name == "Stack<int>").count(), 1);
    assert_eq!(p.structs.iter().filter(|s| s.name == "Stack<string>").count(), 1);
    assert_eq!(p.funcs.iter().filter(|f| f.full_name == "t.Stack<int>.Push").count(), 1);
    // Nothing of the generic declarations themselves is emitted.
    assert!(!p.funcs.iter().any(|f| f.name == "Max" || f.name.contains("<T>")));
    assert!(!p.structs.iter().any(|s| s.name.contains("<T>")));
    no_params_left(&p);
    let dump = p.dump();
    assert!(dump.contains("t.Max<int> [slang_t__Max__int]"), "{dump}");
    assert!(dump.contains("struct #") && dump.contains("t.Stack<int>"), "{dump}");
}

#[test]
fn nested_and_recursive_instantiations() {
    let p = ok("struct Box<T> {\n    V T\n}\nfunc Wrap<T>(x T, n int) int {\n    if n == 0 {\n        return 0\n    }\n    return 1 + Wrap(x, n - 1)\n}\nfunc main() {\n    var b = Box<Box<int>>{V: Box<int>{V: 1}}\n    println(b.V.V, Wrap(\"s\", 3))\n}");
    assert!(p.structs.iter().any(|s| s.name == "Box<Box<int>>"));
    assert!(p.structs.iter().any(|s| s.name == "Box<int>"));
    assert_eq!(funcs_named(&p, "Wrap<string>").len(), 1);
    // A struct whose method returns another instance of itself.
    let p = ok("struct Pair<A, B> {\n    First A\n    Second B\n    func Swap() Pair<B, A> {\n        return Pair<B, A>{First: this.Second, Second: this.First}\n    }\n}\nfunc main() {\n    var p = Pair<int, string>{First: 1, Second: \"a\"}\n    println(p.Swap().First)\n}");
    assert_eq!(p.structs.iter().filter(|s| s.name.starts_with("Pair<")).count(), 2);
    // Infinite expansion is cut off.
    err("func F<T>(x T, n int) int {\n    if n == 0 {\n        return 0\n    }\n    return F([x], n - 1)\n}\nfunc main() {\n    println(F(1, 2))\n}", "nested too deeply");
}

#[test]
fn generic_enums_and_switch() {
    let p = ok("enum Option<T> {\n    Some(value T)\n    None\n}\nfunc Get(o Option<int>) int {\n    switch o {\n        case Option.Some(v):\n            return v\n        case Option<int>.None:\n            return -1\n    }\n    return 0\n}\nfunc main() {\n    println(Get(Option<int>.Some(4)), Get(Option<int>.None))\n    var s = Option<string>.Some(\"x\")\n    switch s {\n        case Option.Some(t):\n            println(t)\n    }\n}");
    let e: Vec<&Enum> = p.enums.iter().filter(|e| e.name.starts_with("Option<")).collect();
    assert_eq!(e.len(), 2);
    let int_opt = e.iter().find(|e| e.name == "Option<int>").unwrap();
    assert_eq!(p.types.get(int_opt.variants[0].fields[0].1), &Type::Int(slang::types::IntKind::I64));
    let get = funcs_named(&p, "Get")[0];
    let StmtKind::Switch(sw) = &get.body.stmts[0].kind else { panic!("switch") };
    assert!(matches!(sw.cases[0].patterns[0], Pattern::Variant { variant: 0, .. }));
    assert!(matches!(sw.cases[1].patterns[0], Pattern::Variant { variant: 1, .. }));
    err("enum Option<T> {\n    Some(value T)\n    None\n}\nfunc main() {\n    var o = Option<int>.Some(\"s\")\n    _ = o\n}", "cannot use string as int");
}

#[test]
fn generic_methods() {
    let p = ok("struct L {\n    P string\n    func Show<T>(x T) string {\n        return this.P\n    }\n    static func Make<T>(x T) L {\n        return L{P: \"m\"}\n    }\n}\nfunc main() {\n    var l = L{P: \"> \"}\n    println(l.Show(1), l.Show<bool>(true), l.Show(2), L.Make(1).P)\n}");
    assert_eq!(p.funcs.iter().filter(|f| f.full_name == "t.L.Show<int>").count(), 1);
    assert_eq!(p.funcs.iter().filter(|f| f.full_name == "t.L.Show<bool>").count(), 1);
    let show = funcs_named(&p, "Show<int>")[0];
    assert!(matches!(show.kind, FuncKind::Method { is_static: false, .. }));
    err("struct L {\n    func Get() int {\n        return 1\n    }\n}\nfunc main() {\n    var l = L{}\n    println(l.Get<int>())\n}", "is not generic");
}

#[test]
fn generic_interfaces_are_rejected() {
    err("interface Box<T> {\n    func Get() T\n}\nfunc main() {\n}", "generic interfaces are not supported");
}

#[test]
fn collections() {
    let p = ok("import std.collections\nfunc main() {\n    var l = List<int>{}\n    l.Add(1)\n    var m = Map<string, int>{}\n    m.Set(\"a\", 1)\n    var s = Set<int>.New()\n    s.Add(1)\n    for x in l {\n        println(x)\n    }\n    for k, v in m {\n        println(k, v)\n    }\n    for i, x in s {\n        println(i, x)\n    }\n    println(l.Contains(1), m.Has(\"a\"), m.Keys(), s.Items())\n}");
    assert!(p.structs.iter().any(|s| s.full_name == "std.collections.List<int>"));
    assert!(p.structs.iter().any(|s| s.full_name == "std.collections.Map<string, int>"));
    no_params_left(&p);
    let main = funcs_named(&p, "main")[0];
    let loops = main.body.stmts.iter().filter(|s| matches!(&s.kind, StmtKind::Block(b) if b.stmts.iter().any(|x| matches!(x.kind, StmtKind::ForSlice { .. })))).count();
    assert_eq!(loops, 3, "collection loops are lowered to slice loops");
    // Element requirements are checked where they are needed.
    err("import std.collections\nstruct P {\n    X int\n}\nfunc main() {\n    var l = List<P>{}\n    l.Add(P{X: 1})\n    println(l.Contains(P{X: 1}))\n}", "required by List<P>.Contains");
    ok("import std.collections\nstruct P {\n    X int\n}\nfunc main() {\n    var l = List<P>{}\n    l.Add(P{X: 1})\n    println(l.Len())\n}");
    err("import std.collections\nstruct P {\n    X int\n}\nfunc main() {\n    var m = Map<P, int>{}\n    _ = m\n}", "does not satisfy constraint Comparable");
    err("import std.collections\nfunc main() {\n    var l = List<int>{}\n    println(len(l.items))\n}", "items is private to namespace std.collections");
}
