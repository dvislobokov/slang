//! `slang build` / `run` / `check` / `test`: front end -> LLVM module ->
//! verification -> optimisation (new pass manager) -> object file(s) ->
//! link with `clang` against `libslang_rt.a`.
//!
//! Two kinds of builds:
//!
//! - **Plain**: a single file or a directory without a manifest. The whole
//!   program becomes one object file, as before.
//! - **Package**: a project found through `slang.toml|json|yaml` (see
//!   [`crate::package`]). Sema runs over the target and its dependency
//!   closure with strict isolation; the resulting module is split into one
//!   module per package ([`split_module`]), each compiled to its own object
//!   `target/slang/<pkg>-<hash>.o`. The hash covers the package's split IR,
//!   the flags and the compiler, so an unchanged package is not recompiled.
//!
//! Every object gets one section per function and global (the equivalent
//! of `-ffunction-sections -fdata-sections`); `--release` links with
//! `-Wl,--gc-sections -s`, `--static` with `-static`.

use crate::codegen;
use crate::diagnostics::Diagnostics;
use crate::driver;
use crate::hir::Program;
use crate::package::{self, lock, PkgId, Project};
use inkwell::context::Context;
use inkwell::module::{Linkage, Module};
use inkwell::passes::PassBuilderOptions;
use inkwell::targets::{FileType, TargetMachine};
use inkwell::types::{AnyTypeEnum, BasicTypeEnum};
use inkwell::values::{AsValueRef, GlobalValue};
use inkwell::{GlobalVisibility, OptimizationLevel};
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::process::Command;

/// What `slang build` produces.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Emit {
    /// A linked executable (default).
    Exe,
    /// Optimised textual LLVM IR (`.ll`).
    LlvmIr,
    /// An object file (`.o`).
    Obj,
}

/// Options of `slang build` (and `run`, `check`, `test`).
#[derive(Clone, Debug)]
pub struct BuildOptions {
    /// File, directory or project directory; `None` = find the project
    /// from the current directory.
    pub input: Option<PathBuf>,
    pub output: Option<PathBuf>,
    pub emit: Emit,
    /// Optimisation level 0..=3.
    pub opt: u8,
    /// Release build: no `-rdynamic`, `--gc-sections`, stripped.
    pub release: bool,
    /// Fully static executable (`clang -static`).
    pub static_link: bool,
    /// Log cache decisions and tool invocations to stderr.
    pub verbose: bool,
    /// Fail instead of updating `slang.lock`.
    pub locked: bool,
    /// Build every workspace member.
    pub workspace: bool,
    /// Build these members (`-p name`).
    pub packages: Vec<String>,
    /// `--leak-check`: at exit the program reports objects still alive
    /// (with their type names unless `release`) and exits non-zero.
    pub leak_check: bool,
}

impl BuildOptions {
    pub fn new(input: impl Into<PathBuf>) -> BuildOptions {
        BuildOptions {
            input: Some(input.into()),
            output: None,
            emit: Emit::Exe,
            opt: 0,
            release: false,
            static_link: false,
            verbose: false,
            locked: false,
            workspace: false,
            packages: Vec::new(),
            leak_check: false,
        }
    }
}

/// Parses `build`/`run`/`check`/`test` flags; returns the options and the
/// remaining arguments after `--`.
pub fn parse_args(args: &[String]) -> Result<(BuildOptions, Vec<String>), String> {
    let mut o = BuildOptions::new("");
    o.input = None;
    let mut opt = None;
    let mut rest = Vec::new();
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        match a.as_str() {
            "--" => {
                rest = args[i + 1..].to_vec();
                break;
            }
            "-o" => {
                i += 1;
                o.output = Some(PathBuf::from(args.get(i).ok_or("-o needs a path")?));
            }
            "-p" | "--package" => {
                i += 1;
                o.packages.push(args.get(i).ok_or("-p needs a package name")?.clone());
            }
            "--release" => o.release = true,
            "--leak-check" => o.leak_check = true,
            "--static" => o.static_link = true,
            "--verbose" | "-v" => o.verbose = true,
            "--locked" => o.locked = true,
            "--workspace" => o.workspace = true,
            "--emit=llvm-ir" => o.emit = Emit::LlvmIr,
            "--emit=obj" => o.emit = Emit::Obj,
            "--emit=exe" => o.emit = Emit::Exe,
            "-O0" | "-O1" | "-O2" | "-O3" => opt = Some(a[2..].parse::<u8>().unwrap()),
            _ if a.starts_with('-') => return Err(format!("unknown option `{a}`")),
            _ => {
                if o.input.is_some() {
                    return Err(format!("unexpected argument `{a}`"));
                }
                o.input = Some(PathBuf::from(a));
            }
        }
        i += 1;
    }
    o.opt = opt.unwrap_or(if o.release { 2 } else { 0 });
    if o.workspace && !o.packages.is_empty() {
        return Err("--workspace and -p are mutually exclusive".into());
    }
    if o.output.is_some() && o.workspace {
        return Err("-o cannot be used with --workspace".into());
    }
    Ok((o, rest))
}

/// Locates `libslang_rt.a`: `$SLANG_RUNTIME` (the archive or its
/// directory), next to the compiler (`<exe>/../../runtime`), then the
/// repository the compiler was built from.
pub fn find_runtime() -> Result<PathBuf, String> {
    let mut cands = Vec::new();
    if let Ok(p) = std::env::var("SLANG_RUNTIME") {
        let p = PathBuf::from(p);
        cands.push(if p.is_dir() { p.join("libslang_rt.a") } else { p });
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            cands.push(dir.join("../../runtime/libslang_rt.a"));
            cands.push(dir.join("../../../runtime/libslang_rt.a"));
        }
    }
    cands.push(Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime/libslang_rt.a"));
    for c in &cands {
        if c.is_file() {
            return Ok(c.canonicalize().unwrap_or(c.clone()));
        }
    }
    Err(format!(
        "cannot find libslang_rt.a (set SLANG_RUNTIME or run `make -C runtime`); looked in: {}",
        cands.iter().map(|c| c.display().to_string()).collect::<Vec<_>>().join(", ")
    ))
}

fn find_clang() -> Result<String, String> {
    for c in ["clang-18", "clang"] {
        if Command::new(c).arg("--version").output().is_ok_and(|o| o.status.success()) {
            return Ok(c.to_string());
        }
    }
    Err("cannot find clang (tried clang-18, clang)".into())
}

/// Default output path of a plain build: the input's stem (or directory
/// name) in the current directory, with the emit kind's extension.
fn default_output(input: &Path, emit: Emit) -> PathBuf {
    let stem = input.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_else(|| "out".into());
    with_emit_ext(PathBuf::from(stem), emit)
}

fn with_emit_ext(mut p: PathBuf, emit: Emit) -> PathBuf {
    match emit {
        Emit::Exe => {}
        Emit::LlvmIr => {
            p.set_extension("ll");
        }
        Emit::Obj => {
            p.set_extension("o");
        }
    }
    p
}

fn opt_level(n: u8) -> OptimizationLevel {
    match n {
        0 => OptimizationLevel::None,
        1 => OptimizationLevel::Less,
        2 => OptimizationLevel::Default,
        _ => OptimizationLevel::Aggressive,
    }
}

fn log(opts: &BuildOptions, msg: impl AsRef<str>) {
    if opts.verbose {
        eprintln!("slang: {}", msg.as_ref());
    }
}

// -- what to build -------------------------------------------------------------

/// The resolved input of a command.
enum Plan {
    /// A single file or a directory without a manifest.
    Plain(PathBuf),
    /// A project and the members to build.
    Project(Project, Vec<PkgId>),
}

fn plan(opts: &BuildOptions) -> Result<Plan, String> {
    let project = match &opts.input {
        Some(p) if p.is_file() => return Ok(Plan::Plain(p.clone())),
        Some(p) if p.is_dir() => {
            if package::manifest::manifest_in(p)?.is_none() {
                if opts.workspace || !opts.packages.is_empty() {
                    return Err(format!("{} has no manifest; --workspace and -p need a project", p.display()));
                }
                return Ok(Plan::Plain(p.clone()));
            }
            package::discover(Some(p))?.expect("manifest exists")
        }
        Some(p) => return Err(format!("cannot read {}: no such file or directory", p.display())),
        None => match package::discover(None)? {
            Some(p) => p,
            None => {
                return Err(format!(
                    "could not find {} in the current directory or any parent; pass a file or directory, or create a project with `slang init`",
                    package::manifest::MANIFEST_NAMES.join(", ")
                ))
            }
        },
    };
    let targets = if opts.workspace {
        project.members.clone()
    } else if !opts.packages.is_empty() {
        let mut t = Vec::new();
        for name in &opts.packages {
            match project.find_member(name) {
                Some(id) => t.push(id),
                None => {
                    let names: Vec<&str> = project.members.iter().map(|&m| project.packages[m].name.as_str()).collect();
                    return Err(format!("no workspace member named `{name}` (members: {})", names.join(", ")));
                }
            }
        }
        t
    } else if let Some(c) = project.current.or(project.root_package) {
        vec![c]
    } else {
        project.members.clone()
    };
    if targets.is_empty() {
        return Err(format!("{}: the workspace has no members", project.root_manifest.path.display()));
    }
    let targets = if opts.output.is_some() && targets.len() > 1 {
        // `-o` names one executable: the only binary among the targets.
        let bins: Vec<PkgId> = targets.iter().copied().filter(|&t| project.packages[t].is_bin()).collect();
        if bins.len() != 1 {
            return Err("-o needs a single package; select one with -p".into());
        }
        bins
    } else {
        targets
    };
    Ok(Plan::Project(project, targets))
}

fn sync_lock(project: &Project, opts: &BuildOptions) -> Result<(), String> {
    if lock::sync(project, opts.locked)? == lock::LockStatus::Written {
        log(opts, format!("wrote {}", project.lock_path().display()));
    }
    Ok(())
}

// -- commands -------------------------------------------------------------------

/// `slang build`: builds every selected target; returns the produced files
/// (executables, or nothing for libraries).
pub fn build(opts: &BuildOptions) -> Result<Vec<PathBuf>, String> {
    match plan(opts)? {
        Plan::Plain(input) => Ok(vec![build_plain(&input, opts)?]),
        Plan::Project(project, targets) => {
            sync_lock(&project, opts)?;
            let mut out = Vec::new();
            for t in targets {
                out.extend(build_package(&project, t, opts)?);
            }
            Ok(out)
        }
    }
}

/// `slang run`: builds the program and runs it; returns its exit code.
pub fn run(mut opts: BuildOptions, args: &[String]) -> Result<i32, String> {
    opts.emit = Emit::Exe;
    let (exe, tmp) = match plan(&opts)? {
        Plan::Plain(input) => {
            let dir = std::env::temp_dir().join(format!("slang-run-{}", std::process::id()));
            std::fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
            opts.output = Some(dir.join("prog"));
            match build_plain(&input, &opts) {
                Ok(exe) => (exe, Some(dir)),
                Err(e) => {
                    let _ = std::fs::remove_dir_all(&dir);
                    return Err(e);
                }
            }
        }
        Plan::Project(project, targets) => {
            let bins: Vec<PkgId> = targets.iter().copied().filter(|&t| project.packages[t].is_bin()).collect();
            let t = match bins.as_slice() {
                [t] => *t,
                [] => return Err("nothing to run: the selected packages are libraries (no `entry`)".into()),
                _ => {
                    let names: Vec<&str> = bins.iter().map(|&b| project.packages[b].name.as_str()).collect();
                    return Err(format!("several binaries ({}); choose one with -p", names.join(", ")));
                }
            };
            sync_lock(&project, &opts)?;
            let exe = build_package(&project, t, &opts)?.ok_or("internal error: binary package produced no executable")?;
            (exe, None)
        }
    };
    let status = Command::new(&exe).args(args).status().map_err(|e| format!("cannot run {}: {e}", exe.display()));
    if let Some(d) = tmp {
        let _ = std::fs::remove_dir_all(d);
    }
    let status = status?;
    use std::os::unix::process::ExitStatusExt;
    Ok(status.code().unwrap_or_else(|| 128 + status.signal().unwrap_or(0)))
}

/// `slang check` (`dev`: `slang test`, dev-dependencies visible): runs the
/// front end; with `dump_hir`, prints the HIR of each target.
pub fn check(opts: &BuildOptions, dump_hir: bool, dev: bool) -> Result<usize, String> {
    match plan(opts)? {
        Plan::Plain(input) => {
            let (files, program) = front_end_plain(&input)?;
            let _ = files;
            if dump_hir {
                print!("{}", program.dump());
            }
            Ok(1)
        }
        Plan::Project(project, targets) => {
            sync_lock(&project, opts)?;
            for &t in &targets {
                let program = front_end_package(&project, t, dev)?.1;
                if dump_hir {
                    print!("{}", program.dump());
                }
            }
            Ok(targets.len())
        }
    }
}

// -- front end ------------------------------------------------------------------

fn front_end_plain(input: &Path) -> Result<(Vec<crate::span::SourceFile>, Program), String> {
    let paths = driver::collect_files(input).map_err(|e| format!("cannot read {}: {e}", input.display()))?;
    if paths.is_empty() {
        return Err(format!("no .sl files in {}", input.display()));
    }
    let files = driver::load(&paths).map_err(|e| format!("cannot read source: {e}"))?;
    let mut diags = Diagnostics::new();
    let program = driver::check(&files, &mut diags);
    if !diags.is_empty() {
        eprint!("{}", diags.render_all_files(&files));
    }
    match program {
        Some(p) if !diags.has_errors() => Ok((files, p)),
        _ => Err("compilation failed".into()),
    }
}

fn front_end_package(project: &Project, target: PkgId, dev: bool) -> Result<(package::Checked, Program), String> {
    let mut diags = Diagnostics::new();
    let mut checked = package::check(project, target, dev, &mut diags)?;
    if !diags.is_empty() {
        eprint!("{}", diags.render_all_files(&checked.files));
    }
    match checked.program.take() {
        Some(p) if !diags.has_errors() => Ok((checked, p)),
        _ => Err(format!("could not compile `{}`", project.packages[target].name)),
    }
}

// -- plain builds -----------------------------------------------------------------

fn build_plain(input: &Path, opts: &BuildOptions) -> Result<PathBuf, String> {
    let (_, program) = front_end_plain(input)?;
    if program.entry.is_none() && opts.emit == Emit::Exe {
        return Err("no `main` function: cannot build an executable".into());
    }
    let output = opts.output.clone().unwrap_or_else(|| default_output(input, opts.emit));
    let ctx = Context::create();
    let name = input.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_else(|| "main".into());
    let module = lower(&program, &ctx, &name, &output, opts)?;
    let machine = codegen::host_machine(opt_level(opts.opt))?;
    set_sections(&module);
    optimise(&module, &machine, opts.opt)?;
    match opts.emit {
        Emit::LlvmIr => {
            module.print_to_file(&output).map_err(|e| format!("cannot write {}: {}", output.display(), e.to_string()))?;
            return Ok(output);
        }
        Emit::Obj => {
            write_object(&machine, &module, &output)?;
            return Ok(output);
        }
        Emit::Exe => {}
    }
    let obj = output.with_extension(format!("{}.o", std::process::id()));
    write_object(&machine, &module, &obj)?;
    let r = link(&[obj.clone()], &output, opts);
    let _ = std::fs::remove_file(&obj);
    r?;
    Ok(output)
}

/// Codegen + verification of the whole program.
fn lower<'ctx>(program: &Program, ctx: &'ctx Context, name: &str, output: &Path, opts: &BuildOptions) -> Result<Module<'ctx>, String> {
    let leak_check = match (opts.leak_check, opts.release) {
        (false, _) => 0,
        (true, true) => 1,
        (true, false) => 2,
    };
    let module = match codegen::emit_with(program, ctx, name, codegen::Options { leak_check }) {
        Ok(m) => m,
        Err(errs) => {
            for e in &errs {
                eprintln!("error: {e}");
            }
            return Err("code generation failed".into());
        }
    };
    if let Err(e) = module.verify() {
        let ll = output.with_extension("ll");
        let _ = module.print_to_file(&ll);
        return Err(format!("internal compiler error: LLVM module verification failed (IR written to {}):\n{}", ll.display(), e.to_string()));
    }
    Ok(module)
}

fn optimise(module: &Module, machine: &TargetMachine, opt: u8) -> Result<(), String> {
    let pipeline = format!("default<O{}>", opt.min(3));
    module.run_passes(&pipeline, machine, PassBuilderOptions::create()).map_err(|e| format!("internal compiler error: optimisation failed: {}", e.to_string()))
}

fn write_object(machine: &TargetMachine, module: &Module, path: &Path) -> Result<(), String> {
    machine.write_to_file(module, FileType::Object, path).map_err(|e| format!("cannot write {}: {}", path.display(), e.to_string()))
}

/// Links `objs` with the runtime into the executable `output`.
fn link(objs: &[PathBuf], output: &Path, opts: &BuildOptions) -> Result<(), String> {
    let rt = find_runtime()?;
    let clang = find_clang()?;
    let mut cmd = Command::new(&clang);
    cmd.args(objs).arg(&rt).arg("-lpthread").arg("-lm").arg("-o").arg(output);
    if opts.release {
        cmd.arg("-Wl,--gc-sections").arg("-s");
    } else if !opts.static_link {
        cmd.arg("-rdynamic");
    }
    if opts.static_link {
        cmd.arg("-static");
    }
    log(opts, format!("linking {} ({} object(s))", output.display(), objs.len()));
    let out = cmd.output().map_err(|e| format!("cannot run {clang}: {e}"))?;
    if !out.status.success() {
        return Err(format!("linking failed:\n{}", String::from_utf8_lossy(&out.stderr)));
    }
    Ok(())
}

// -- package builds ---------------------------------------------------------------

/// Builds member `target`: its executable (binary) or just the objects of
/// its closure (library, returns `None`).
fn build_package(project: &Project, target: PkgId, opts: &BuildOptions) -> Result<Option<PathBuf>, String> {
    let pkg = &project.packages[target];
    let (checked, program) = front_end_package(project, target, false)?;
    let profile = if opts.release { "release" } else { "debug" };
    let out_dir = project.target_dir().join(profile);
    let output = opts.output.clone().unwrap_or_else(|| with_emit_ext(out_dir.join(&pkg.name), opts.emit));
    if let Some(dir) = output.parent().filter(|d| !d.as_os_str().is_empty()) {
        std::fs::create_dir_all(dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    }
    let ctx = Context::create();
    let module = lower(&program, &ctx, &pkg.name, &output, opts)?;
    let machine = codegen::host_machine(opt_level(opts.opt))?;
    if opts.emit != Emit::Exe {
        // Whole-program artefacts are not split.
        set_sections(&module);
        optimise(&module, &machine, opts.opt)?;
        if opts.emit == Emit::LlvmIr {
            module.print_to_file(&output).map_err(|e| format!("cannot write {}: {}", output.display(), e.to_string()))?;
        } else {
            write_object(&machine, &module, &output)?;
        }
        return Ok(Some(output));
    }
    let cache = project.target_dir().join("slang");
    std::fs::create_dir_all(&cache).map_err(|e| format!("cannot create {}: {e}", cache.display()))?;
    let compiler = compiler_id();
    let mut objs = Vec::new();
    for (i, &id) in checked.closure.iter().enumerate() {
        let name = &project.packages[id].name;
        let part = split_module(&module, &program, &checked.file_pkg, i, name)?;
        set_sections(&part);
        let mut h = Sha256::new();
        h.update(compiler.as_bytes());
        h.update(format!("\0opt={}\0release={}\0leak={}\0triple={}\0", opts.opt, opts.release, opts.leak_check, machine.get_triple()).as_bytes());
        h.update(part.print_to_string().to_bytes());
        let key = lock::hex(&h.finalize()[..8]);
        let obj = cache.join(format!("{name}-{key}.o"));
        if obj.is_file() {
            log(opts, format!("cache hit: {name} ({})", obj.display()));
        } else {
            log(opts, format!("compiling {name} -> {}", obj.display()));
            optimise(&part, &machine, opts.opt)?;
            let tmp = obj.with_extension(format!("{}.tmp", std::process::id()));
            write_object(&machine, &part, &tmp)?;
            std::fs::rename(&tmp, &obj).map_err(|e| format!("cannot write {}: {e}", obj.display()))?;
        }
        objs.push(obj);
    }
    if !pkg.is_bin() {
        log(opts, format!("library {} checked and compiled ({} object(s))", package::describe(pkg), objs.len()));
        return Ok(None);
    }
    link(&objs, &output, opts)?;
    Ok(Some(output))
}

/// A digest identifying this compiler binary (version, size, mtime), so a
/// rebuilt compiler does not reuse objects of an older one.
fn compiler_id() -> String {
    let mut id = format!("slang {}", env!("CARGO_PKG_VERSION"));
    if let Some(m) = std::env::current_exe().ok().and_then(|e| std::fs::metadata(e).ok()) {
        let mtime = m.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map(|d| d.as_nanos()).unwrap_or(0);
        id.push_str(&format!(" {} {mtime}", m.len()));
    }
    id
}

/// The part of the whole-program `module` that belongs to package `pkg`
/// (a position in the build closure; `file_pkg` maps HIR files to it):
///
/// - user functions and globals of `pkg` keep their bodies/initialisers
///   and become external symbols (`slang_<namespace>__<name>` is unique
///   because namespace roots are unique);
/// - those of other packages become external declarations;
/// - `main` stays only in the package that defines the entry point;
/// - compiler-generated helpers (type infos, vtables, rc/drop helpers,
///   thunks) stay in every part as `linkonce_odr` in a comdat, so the
///   linker keeps one copy and run-time type identity (the address of a
///   type info) holds across objects. Names agree because every part is
///   cut from the same module, and a cached object is reused only when its
///   IR is byte-identical to the current part. Unreferenced helpers are
///   then dropped by `globaldce`.
fn split_module<'ctx>(module: &Module<'ctx>, program: &Program, file_pkg: &[usize], pkg: usize, name: &str) -> Result<Module<'ctx>, String> {
    let part = module.clone();
    part.set_name(name);
    part.set_source_file_name(name);
    let mut user = std::collections::HashSet::new();
    for f in &program.funcs {
        user.insert(f.symbol.clone());
        let Some(fv) = part.get_function(&f.symbol) else { continue };
        if file_pkg[f.file] == pkg {
            fv.set_linkage(Linkage::External);
        } else if fv.count_basic_blocks() > 0 {
            // Replace the definition by a declaration of the same type.
            let decl = part.add_function(&format!("{}.decl", f.symbol), fv.get_type(), Some(Linkage::External));
            fv.replace_all_uses_with(decl);
            unsafe { fv.delete() };
            decl.as_global_value().set_name(&f.symbol);
        }
    }
    for g in &program.globals {
        user.insert(g.symbol.clone());
        let Some(gv) = part.get_global(&g.symbol) else { continue };
        gv.set_linkage(Linkage::External);
        if file_pkg[g.file] != pkg {
            // SAFETY: a null initializer turns the definition into a declaration.
            unsafe { llvm_sys::core::LLVMSetInitializer(gv.as_value_ref(), std::ptr::null_mut()) };
        }
    }
    let entry_pkg = program.entry.map(|e| file_pkg[program.funcs[e.0 as usize].file]);
    if entry_pkg != Some(pkg) {
        if let Some(main) = part.get_function("main") {
            unsafe { main.delete() };
        }
    }
    let make_shared = |gv: GlobalValue<'ctx>| {
        let n = gv.get_name().to_string_lossy().to_string();
        if gv.is_declaration() || user.contains(&n) || gv.get_linkage() != Linkage::Internal {
            return;
        }
        gv.set_linkage(Linkage::LinkOnceODR);
        gv.set_visibility(GlobalVisibility::Hidden);
        gv.set_comdat(part.get_or_insert_comdat(&n));
    };
    for f in part.get_functions() {
        make_shared(f.as_global_value());
    }
    for g in part.get_globals() {
        make_shared(g);
    }
    let machine = codegen::host_machine(OptimizationLevel::None)?;
    part.run_passes("globaldce", &machine, PassBuilderOptions::create()).map_err(|e| format!("internal compiler error: globaldce failed: {}", e.to_string()))?;
    if let Err(e) = part.verify() {
        return Err(format!("internal compiler error: split module of `{name}` is invalid:\n{}", e.to_string()));
    }
    Ok(part)
}

/// One section per defined function and global (`.text.<name>`,
/// `.data.<name>`, `.bss.<name>`, `.rodata.<name>`, `.data.rel.ro.<name>`),
/// like clang's `-ffunction-sections -fdata-sections`, so the linker's
/// `--gc-sections` can drop unused code and data.
fn set_sections(module: &Module) {
    for f in module.get_functions() {
        if f.count_basic_blocks() > 0 {
            let n = f.get_name().to_string_lossy().to_string();
            f.set_section(Some(&format!(".text.{n}")));
        }
    }
    for g in module.get_globals() {
        if g.is_declaration() {
            continue;
        }
        let n = g.get_name().to_string_lossy().to_string();
        if n.starts_with("llvm.") {
            continue;
        }
        let zero = g.get_initializer().is_some_and(|i| unsafe { llvm_sys::core::LLVMIsNull(i.as_value_ref()) != 0 });
        let prefix = if g.is_constant() {
            if has_pointers(g.get_value_type()) {
                ".data.rel.ro"
            } else {
                ".rodata"
            }
        } else if zero {
            ".bss"
        } else {
            ".data"
        };
        g.set_section(Some(&format!("{prefix}.{n}")));
    }
}

/// Whether values of `t` may hold addresses (which need relocations, so
/// read-only data goes to `.data.rel.ro`).
fn has_pointers(t: AnyTypeEnum) -> bool {
    fn basic(t: BasicTypeEnum) -> bool {
        match t {
            BasicTypeEnum::PointerType(_) => true,
            BasicTypeEnum::StructType(s) => s.get_field_types().into_iter().any(basic),
            BasicTypeEnum::ArrayType(a) => basic(a.get_element_type()),
            BasicTypeEnum::VectorType(v) => basic(v.get_element_type()),
            BasicTypeEnum::IntType(_) | BasicTypeEnum::FloatType(_) => false,
        }
    }
    match BasicTypeEnum::try_from(t) {
        Ok(b) => basic(b),
        Err(_) => true,
    }
}
