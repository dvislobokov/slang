//! Package manager, stage 1: manifests, workspaces, path dependencies,
//! strict isolation, namespace roots, cycles, `slang.lock`, the object
//! cache and `slang init`. Fixtures live in `tests/fixtures/pkg/`; every
//! test works on its own copy (builds write `target/` and `slang.lock`).
//!
//! Needs `clang` and `runtime/libslang_rt.a` for the tests that link.

use slang::package::manifest::{self, Format};
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::atomic::{AtomicUsize, Ordering};

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

fn copy_dir(from: &Path, to: &Path) {
    std::fs::create_dir_all(to).unwrap();
    for e in std::fs::read_dir(from).unwrap() {
        let e = e.unwrap();
        let p = e.path();
        let name = e.file_name();
        if name == "target" || name == "slang.lock" {
            continue;
        }
        if p.is_dir() {
            copy_dir(&p, &to.join(&name));
        } else {
            std::fs::copy(&p, to.join(&name)).unwrap();
        }
    }
}

/// A fresh copy of all fixtures; returns its root.
fn fixtures(test: &str) -> PathBuf {
    static N: AtomicUsize = AtomicUsize::new(0);
    let d = std::env::temp_dir().join(format!("slang-pkg-{}-{}-{test}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
    let _ = std::fs::remove_dir_all(&d);
    copy_dir(&Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/pkg"), &d);
    d
}

fn slang(dir: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_slang")).args(args).current_dir(dir).output().expect("cannot run slang")
}

fn ok(dir: &Path, args: &[&str]) -> (String, String) {
    let o = slang(dir, args);
    let (out, err) = (String::from_utf8_lossy(&o.stdout).to_string(), String::from_utf8_lossy(&o.stderr).to_string());
    assert!(o.status.success(), "slang {args:?} in {} failed:\nstdout:\n{out}\nstderr:\n{err}", dir.display());
    (out, err)
}

fn fails(dir: &Path, args: &[&str]) -> String {
    let o = slang(dir, args);
    let err = String::from_utf8_lossy(&o.stderr).to_string();
    assert!(!o.status.success(), "slang {args:?} in {} unexpectedly succeeded:\n{err}", dir.display());
    err
}

const APP_OUTPUT: &str = "sum is 42\ntwice: 42\nsquare 3 area 9\nrect 2x5 area 10\ncalls: 2\n";

#[test]
fn workspace_of_three_packages_builds_and_runs() {
    ensure_runtime();
    let root = fixtures("ws");
    let ws = root.join("workspace");
    // From a member directory: discovery walks up to the workspace root.
    let (out, _) = ok(&ws.join("app/src"), &["run"]);
    assert_eq!(out, APP_OUTPUT);
    assert!(ws.join("slang.lock").is_file(), "lock file at the workspace root");
    assert!(ws.join("target/debug/app").is_file());
    // `-p` and `--workspace` from the (virtual) root.
    let (out, _) = ok(&ws, &["run", "-p", "app"]);
    assert_eq!(out, APP_OUTPUT);
    ok(&ws, &["build", "--workspace"]);
    ok(&ws, &["check", "-p", "lib-a"]);
    let err = fails(&ws, &["build", "-p", "nope"]);
    assert!(err.contains("no workspace member named `nope`"), "{err}");
    // A release build of the same graph, linked statically.
    ok(&ws, &["build", "-p", "app", "--release", "--static"]);
    let exe = ws.join("target/release/app");
    let o = Command::new(&exe).output().unwrap();
    assert_eq!(String::from_utf8_lossy(&o.stdout), APP_OUTPUT);
}

#[test]
fn importing_an_undeclared_transitive_dependency_is_an_error() {
    let root = fixtures("phantom");
    let err = fails(&root.join("phantom/app"), &["check"]);
    assert!(err.contains("package `app` cannot use namespace `libb.math`"), "{err}");
    assert!(err.contains("not a declared dependency of `app`"), "{err}");
    assert!(err.contains("through `lib-a`"), "{err}");
    assert!(err.contains("add `lib-b` to [dependencies]"), "{err}");

    // A fully qualified path without `import` is checked too.
    let main = root.join("phantom/app/src/main.sl");
    std::fs::write(&main, "namespace app\n\nimport std.io\nimport liba\n\nfunc main() {\n    println(libb.math.Add(1, 2))\n}\n").unwrap();
    let err = fails(&root.join("phantom/app"), &["check"]);
    assert!(err.contains("cannot use namespace `libb`"), "{err}");

    // Declaring the dependency fixes it.
    let m = root.join("phantom/app/slang.toml");
    let text = std::fs::read_to_string(&m).unwrap() + "lib-b = { path = \"../../workspace/lib-b\" }\n";
    std::fs::write(&m, text).unwrap();
    ok(&root.join("phantom/app"), &["check"]);
}

#[test]
fn namespace_outside_the_package_root_is_an_error() {
    let root = fixtures("ns");
    let err = fails(&root.join("ns-mismatch"), &["check"]);
    assert!(err.contains("namespace `store.models` is outside package `shop`"), "{err}");
    assert!(err.contains("namespace shop.<sub>"), "{err}");
    assert!(err.contains("models.sl:1:11"), "{err}");
}

#[test]
fn duplicate_namespace_roots_are_an_error() {
    let root = fixtures("dup");
    let err = fails(&root.join("dup-root"), &["check", "--workspace"]);
    assert!(err.contains("both own the namespace root `common`"), "{err}");
}

#[test]
fn dependency_cycle_is_an_error() {
    let root = fixtures("cycle");
    let err = fails(&root.join("cycle/a"), &["check"]);
    assert!(err.contains("dependency cycle: a -> b -> a"), "{err}");
}

#[test]
fn several_manifests_are_an_error() {
    let root = fixtures("two");
    let err = fails(&root.join("two-manifests"), &["check"]);
    assert!(err.contains("several manifests (slang.toml, slang.json)"), "{err}");
}

#[test]
fn manifest_formats_round_trip_through_convert() {
    let root = fixtures("formats");
    let dir = root.join("formats");
    let original = manifest::load_dir(&dir).unwrap().manifest;
    assert!(original.package.as_ref().unwrap().license.is_some());
    assert_eq!(original.features.len(), 3);
    for (fmt, file) in [("json", "slang.json"), ("yaml", "slang.yaml"), ("toml", "slang.toml"), ("yaml", "slang.yaml"), ("json", "slang.json")] {
        ok(&dir, &["manifest", "convert", fmt]);
        assert_eq!(manifest::manifests_in(&dir), vec![dir.join(file)], "exactly one manifest after converting to {fmt}");
        let lm = manifest::load_dir(&dir).unwrap();
        assert_eq!(lm.format, Format::parse(fmt).unwrap());
        assert_eq!(lm.manifest, original, "content preserved after converting to {fmt}");
        ok(&dir, &["check"]);
    }
    // Library-level round trip too, for every pair of formats.
    for a in [Format::Toml, Format::Json, Format::Yaml] {
        let text = original.to_string(a).unwrap();
        assert_eq!(slang::package::Manifest::parse(&text, a).unwrap(), original, "{a}");
    }
}

#[test]
fn manifest_schema_is_json_schema() {
    let root = fixtures("schema");
    let (out, _) = ok(&root, &["manifest", "schema"]);
    let v: serde_json::Value = serde_json::from_str(&out).unwrap();
    assert!(v["$schema"].as_str().unwrap().contains("json-schema.org"));
    for key in ["package", "dependencies", "dev-dependencies", "features", "workspace"] {
        assert!(v["properties"][key].is_object(), "schema has {key}");
    }
}

#[test]
fn lock_file_is_deterministic() {
    let a = fixtures("lock-a");
    let b = fixtures("lock-b");
    ok(&a.join("workspace"), &["check", "--workspace"]);
    ok(&b.join("workspace/lib-a"), &["check"]);
    let la = std::fs::read_to_string(a.join("workspace/slang.lock")).unwrap();
    let lb = std::fs::read_to_string(b.join("workspace/slang.lock")).unwrap();
    assert_eq!(la, lb, "same graph, same lock, wherever it lives and whatever was built");
    // Sorted packages, relative path sources, checksums.
    let names: Vec<&str> = la.lines().filter_map(|l| l.strip_prefix("  name: ")).collect();
    assert_eq!(names, ["app", "lib-a", "lib-b"]);
    assert!(la.contains("source: path:lib-b"), "{la}");
    assert!(la.contains("checksum: sha256:"), "{la}");
    assert!(!la.contains(a.to_str().unwrap()), "no absolute paths: {la}");
    // Re-running does not touch it.
    let before = std::fs::metadata(a.join("workspace/slang.lock")).unwrap().modified().unwrap();
    let (_, err) = ok(&a.join("workspace"), &["check", "--workspace", "--verbose"]);
    assert!(!err.contains("wrote"), "{err}");
    assert_eq!(std::fs::metadata(a.join("workspace/slang.lock")).unwrap().modified().unwrap(), before);
}

#[test]
fn locked_fails_when_the_lock_would_change() {
    let root = fixtures("locked");
    let ws = root.join("workspace");
    let err = fails(&ws, &["check", "--workspace", "--locked"]);
    assert!(err.contains("slang.lock needs to be updated (it does not exist)"), "{err}");
    ok(&ws, &["check", "--workspace"]);
    ok(&ws, &["check", "--workspace", "--locked"]);
    // A manifest change (new version) ...
    let m = ws.join("lib-b/slang.toml");
    std::fs::write(&m, std::fs::read_to_string(&m).unwrap().replace("1.0.0", "1.1.0")).unwrap();
    let err = fails(&ws, &["check", "--workspace", "--locked"]);
    assert!(err.contains("version of `lib-b` changed") && err.contains("--locked"), "{err}");
    ok(&ws, &["check", "--workspace"]);
    // ... and a source change both need a new lock.
    let f = ws.join("lib-b/src/math/add.sl");
    std::fs::write(&f, std::fs::read_to_string(&f).unwrap() + "\n// edited\n").unwrap();
    let err = fails(&ws, &["check", "--workspace", "--locked"]);
    assert!(err.contains("sources (checksum) of `lib-b` changed"), "{err}");
}

#[test]
fn unchanged_packages_are_not_recompiled() {
    ensure_runtime();
    let root = fixtures("cache");
    let app = root.join("workspace/app");
    let (_, first) = ok(&app, &["build", "--verbose"]);
    for p in ["lib-b", "lib-a", "app"] {
        assert!(first.contains(&format!("compiling {p} -> ")), "{first}");
    }
    let objs: Vec<_> = std::fs::read_dir(root.join("workspace/target/slang")).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().to_string()).collect();
    assert_eq!(objs.len(), 3, "{objs:?}");
    assert!(objs.iter().all(|o| o.ends_with(".o")), "{objs:?}");

    let (_, second) = ok(&app, &["build", "--verbose"]);
    assert!(!second.contains("compiling"), "second build recompiled:\n{second}");
    for p in ["lib-b", "lib-a", "app"] {
        assert!(second.contains(&format!("cache hit: {p} (")), "{second}");
    }

    // Editing the binary recompiles only the binary.
    let main = app.join("src/main.sl");
    std::fs::write(&main, std::fs::read_to_string(&main).unwrap().replace("calls:", "calls so far:")).unwrap();
    let (_, third) = ok(&app, &["build", "--verbose"]);
    assert!(third.contains("cache hit: lib-b (") && third.contains("cache hit: lib-a ("), "{third}");
    assert!(third.contains("compiling app -> "), "{third}");
    let o = Command::new(root.join("workspace/target/debug/app")).output().unwrap();
    assert!(String::from_utf8_lossy(&o.stdout).contains("calls so far: 2"));

    // Different flags are a different cache entry.
    let (_, rel) = ok(&app, &["build", "--release", "--verbose"]);
    assert!(rel.contains("compiling lib-b -> "), "{rel}");
}

#[test]
fn init_creates_runnable_packages_in_every_format() {
    ensure_runtime();
    let root = fixtures("init");
    for fmt in ["toml", "json", "yaml"] {
        let name = format!("hello-{fmt}");
        ok(&root, &["init", &name, "--format", fmt]);
        let dir = root.join(&name);
        assert!(dir.join(format!("slang.{fmt}")).is_file());
        let (out, _) = ok(&dir, &["run"]);
        assert_eq!(out, "Hello, world!\n");
    }
    ok(&root, &["init", "util", "--lib"]);
    assert!(root.join("util/src/lib.sl").is_file());
    let lm = manifest::load_dir(&root.join("util")).unwrap();
    assert!(lm.manifest.package.unwrap().entry.is_none(), "a library has no entry");
    ok(&root.join("util"), &["build"]);
    let err = fails(&root.join("util"), &["run"]);
    assert!(err.contains("libraries"), "{err}");
    let err = fails(&root, &["init", "util"]);
    assert!(err.contains("already has a manifest"), "{err}");
}

#[test]
fn plain_directory_without_manifest_still_builds() {
    ensure_runtime();
    let out_dir = fixtures("plain");
    let exe = out_dir.join("multi");
    let multi = repo().join("examples/multi");
    ok(&out_dir, &["build", multi.to_str().unwrap(), "-o", exe.to_str().unwrap()]);
    let o = Command::new(&exe).output().unwrap();
    let want = std::fs::read_to_string(multi.join("namespaces.out")).unwrap();
    assert_eq!(String::from_utf8_lossy(&o.stdout), want);
    // No manifest anywhere up from a temp dir without a path: a clear error.
    let empty = out_dir.join("empty");
    std::fs::create_dir_all(&empty).unwrap();
    if manifest::find_upwards(&empty).unwrap().is_none() {
        let err = fails(&empty, &["build"]);
        assert!(err.contains("could not find slang.toml, slang.json, slang.yaml"), "{err}");
    }
}
