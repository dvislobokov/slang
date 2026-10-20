//! `slang build` / `slang run`: front end -> LLVM module -> verification ->
//! optimisation (new pass manager) -> object file -> link with `clang`
//! against `libslang_rt.a`.

use crate::codegen;
use crate::diagnostics::Diagnostics;
use crate::driver;
use inkwell::context::Context;
use inkwell::passes::PassBuilderOptions;
use inkwell::targets::FileType;
use inkwell::OptimizationLevel;
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

/// Options of `slang build`.
#[derive(Clone, Debug)]
pub struct BuildOptions {
    pub input: PathBuf,
    pub output: Option<PathBuf>,
    pub emit: Emit,
    /// Optimisation level 0..=3.
    pub opt: u8,
    /// Release build: no `-rdynamic`.
    pub release: bool,
}

impl BuildOptions {
    pub fn new(input: impl Into<PathBuf>) -> BuildOptions {
        BuildOptions { input: input.into(), output: None, emit: Emit::Exe, opt: 0, release: false }
    }
}

/// Parses `build`/`run` flags; returns the options and the remaining
/// arguments after `--`.
pub fn parse_args(args: &[String]) -> Result<(BuildOptions, Vec<String>), String> {
    let mut input = None;
    let mut output = None;
    let mut emit = Emit::Exe;
    let mut opt = None;
    let mut release = false;
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
                output = Some(PathBuf::from(args.get(i).ok_or("-o needs a path")?));
            }
            "--release" => release = true,
            "--emit=llvm-ir" => emit = Emit::LlvmIr,
            "--emit=obj" => emit = Emit::Obj,
            "--emit=exe" => emit = Emit::Exe,
            "-O0" | "-O1" | "-O2" | "-O3" => opt = Some(a[2..].parse::<u8>().unwrap()),
            _ if a.starts_with('-') => return Err(format!("unknown option `{a}`")),
            _ => {
                if input.is_some() {
                    return Err(format!("unexpected argument `{a}`"));
                }
                input = Some(PathBuf::from(a));
            }
        }
        i += 1;
    }
    let input = input.ok_or("missing input file or directory")?;
    let opt = opt.unwrap_or(if release { 2 } else { 0 });
    Ok((BuildOptions { input, output, emit, opt, release }, rest))
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

/// Default output path: the input's stem (or directory name) in the
/// current directory, with the emit kind's extension.
fn default_output(input: &Path, emit: Emit) -> PathBuf {
    let stem = input.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_else(|| "out".into());
    let mut p = PathBuf::from(stem);
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

/// Runs the whole pipeline; returns the path of the produced file.
/// Diagnostics are printed to stderr; the error is a one-line summary.
pub fn build(opts: &BuildOptions) -> Result<PathBuf, String> {
    let paths = driver::collect_files(&opts.input).map_err(|e| format!("cannot read {}: {e}", opts.input.display()))?;
    if paths.is_empty() {
        return Err(format!("no .sl files in {}", opts.input.display()));
    }
    let files = driver::load(&paths).map_err(|e| format!("cannot read source: {e}"))?;
    let mut diags = Diagnostics::new();
    let program = driver::check(&files, &mut diags);
    if !diags.is_empty() {
        eprint!("{}", diags.render_all_files(&files));
    }
    let program = match program {
        Some(p) if !diags.has_errors() => p,
        _ => return Err("compilation failed".into()),
    };
    if program.entry.is_none() && opts.emit == Emit::Exe {
        return Err("no `main` function: cannot build an executable".into());
    }
    let output = opts.output.clone().unwrap_or_else(|| default_output(&opts.input, opts.emit));
    let ctx = Context::create();
    let name = opts.input.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_else(|| "main".into());
    let module = match codegen::emit(&program, &ctx, &name) {
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
    let machine = codegen::host_machine(opt_level(opts.opt))?;
    let pipeline = format!("default<O{}>", opts.opt.min(3));
    module
        .run_passes(&pipeline, &machine, PassBuilderOptions::create())
        .map_err(|e| format!("internal compiler error: optimisation failed: {}", e.to_string()))?;
    match opts.emit {
        Emit::LlvmIr => {
            module.print_to_file(&output).map_err(|e| format!("cannot write {}: {}", output.display(), e.to_string()))?;
            return Ok(output);
        }
        Emit::Obj => {
            machine.write_to_file(&module, FileType::Object, &output).map_err(|e| format!("cannot write {}: {}", output.display(), e.to_string()))?;
            return Ok(output);
        }
        Emit::Exe => {}
    }
    let obj = output.with_extension(format!("{}.o", std::process::id()));
    machine.write_to_file(&module, FileType::Object, &obj).map_err(|e| format!("cannot write {}: {}", obj.display(), e.to_string()))?;
    let rt = find_runtime()?;
    let clang = find_clang()?;
    let mut cmd = Command::new(&clang);
    cmd.arg(&obj).arg(&rt).arg("-lpthread").arg("-lm").arg("-o").arg(&output);
    if !opts.release {
        cmd.arg("-rdynamic");
    }
    let out = cmd.output().map_err(|e| format!("cannot run {clang}: {e}"));
    let _ = std::fs::remove_file(&obj);
    let out = out?;
    if !out.status.success() {
        return Err(format!("linking failed:\n{}", String::from_utf8_lossy(&out.stderr)));
    }
    Ok(output)
}

/// `slang run`: builds into a temporary directory and runs the program;
/// returns its exit code.
pub fn run(mut opts: BuildOptions, args: &[String]) -> Result<i32, String> {
    let dir = std::env::temp_dir().join(format!("slang-run-{}", std::process::id()));
    std::fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    opts.emit = Emit::Exe;
    opts.output = Some(dir.join("prog"));
    let exe = build(&opts);
    let r = exe.and_then(|exe| Command::new(&exe).args(args).status().map_err(|e| format!("cannot run {}: {e}", exe.display())));
    let _ = std::fs::remove_dir_all(&dir);
    let status = r?;
    use std::os::unix::process::ExitStatusExt;
    Ok(status.code().unwrap_or_else(|| 128 + status.signal().unwrap_or(0)))
}
