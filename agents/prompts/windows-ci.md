# Prompt: Windows and the self-contained toolchain, run locally

Run on a Windows x64 machine with: Git, Rust (stable, MSVC host), the
official `clang+llvm-18.1.8-x86_64-pc-windows-msvc` tarball (or MSYS2 with
LLVM 18), MSYS2 (`mingw-w64-x86_64-toolchain`, `make`) for the runtime,
PowerShell, and ideally a second clean Windows VM or Sandbox to test the
archive on. Start `claude` in the repository root (`master` at or after
commit d7ec023) and paste the prompt below. Use Opus subagents for
independent parts; verify everything you can locally before pushing.

Background: on 2026-10-22 the first real CI runs on Windows and macOS
happened. Windows: the runtime and its C tests pass natively (MinGW ABI),
the compiler builds against the official MSVC LLVM 18 tarball, the release
dry run produced `zell-0.1.0-windows-amd64.tar.gz`, and the `ci`
windows-amd64 job now runs the Rust test suites for the first time: `lib`
and `cross` pass, later suites fail one by one (first: `tests/data.rs`
migrations_cli_sqlite, fixed by d7ec023; more will follow). The
self-contained bundle (`bundle.yml`: `bin/lld`, `targets/<triple>/`
link inputs, no clang needed) is green when built on Linux and its
Windows executables run on the Windows runner; building the bundle on a
Windows host itself has not been done yet. Open items on other hosts are
listed at the end; do not try to fix macOS from Windows.

---

Read CLAUDE.md, docs/PLATFORMS.md (sections "Дистрибутив", "Релизы"),
.github/workflows/{ci.yml,bundle.yml,release.yml}, scripts/
{package-toolchain.sh,bundle-targets.sh,build-lld.sh,install.ps1},
compiler/src/{crosslink.rs,build.rs,toolchain.rs}, runtime/Makefile,
compiler/tests/cross.rs (the `bundle::*` tests) and the latest runs at
https://github.com/zell-lang/zell/actions (use `gh run list`, `gh run
view --log-failed`). Goal: Windows is a first-class host: every `ci`
windows-amd64 step green, the self-contained toolchain archive built and
tested on Windows itself, the release dry run green on windows-amd64, and
a clean Windows VM able to unpack the archive and build/run programs for
windows-amd64 and cross-compile for linux/macOS with nothing else
installed.

1. **Make `cargo test --workspace` green on Windows.** Run it locally
   (MSVC toolchain, `LLVM_SYS_180_PREFIX` pointing at the official
   tarball; the runtime from MSYS2 `make -C runtime`, copied to
   runtime/libzell_rt.a as ci.yml does). Fix each failure at the root:
   path separators and `\r\n` in byte-exact comparisons (`.gitattributes`
   already forces LF), temp-dir and process handling, `zell test --json`
   and `zell doc` on Windows, the debugger tests (lldb from the LLVM
   tarball), sockets/TLS/HTTP tests (Winsock specifics, `somaxconn`), file
   and permission semantics, console output encoding. Platform-specific
   runtime code belongs in runtime/src/platform/ only. Record any
   Windows-only behaviour decision in examples/README.md or
   tests/spec/README.md; a spec that cannot apply on Windows gets the
   `// os: linux` directive (see tests/spec/README.md).
2. **Build the self-contained bundle on a Windows host.** Make
   scripts/bundle-targets.sh, build-lld.sh and package-toolchain.sh work
   under MSYS2/Git Bash on Windows (or add PowerShell equivalents) so
   `zell-<version>-windows-amd64` contains: `bin\zell.exe`, `zell-lsp.exe`,
   `zell-mcp.exe`, `bin\lld.exe` (built from the LLVM tarball's static
   libs or from source; must depend only on Windows system DLLs: check
   with `dumpbin /dependents` or `llvm-objdump --private-headers`),
   `bin\vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll` (the
   compiler is built against the /MD LLVM), `targets\x86_64-pc-windows-gnu\`
   (mingw-w64 CRT, UCRT and import libraries, compiler-rt builtins,
   libzell_rt.a) and the link inputs of every other tier-1/2 target
   (glibc stubs, musl, macOS tbd, runtimes) so cross-compilation works from
   Windows, `std\`, `THIRD_PARTY.md`. `zell doctor` must pass from the
   unpacked archive.
3. **Test the archive on a clean Windows machine** (a VM or Windows
   Sandbox with nothing installed): `install.ps1 --from <archive>`, then
   `zell run examples\hello.zl`, `zell build --release examples\http_server.zl`,
   `zell test` on a `zell init` project, `zell build --target linux-amd64`
   and `--target macos-arm64` of hello.zl (check the file types), the
   VS Code extension against `zell-lsp.exe`. Script this as
   scripts/bundle-smoke.ps1 and add a `bundle-windows-host` job to
   bundle.yml that builds the bundle on `windows-latest` and runs the
   smoke script.
4. **release.yml**: the windows-amd64 job must produce the self-contained
   archive from step 2 (today it packages the thin toolchain); keep
   `.sha256`, `SHA256SUMS`, `VERSION`; run the dry run
   (`gh workflow run release.yml -f tag=v0.1.0`) and get the Windows job
   green.
5. **`ci.yml` windows-amd64**: keep it honest and fast (cache the LLVM
   tarball, MSYS2 packages and cargo), and add the lldb tests once step 1
   has them passing.
6. Push to `master` in logical commits (English imperative; use
   `[skip ci]` for commits that do not need a run; the runner queue is
   small) and watch the runs with `gh`. Report: what was failing on
   Windows and why, the archive layout and size, the clean-VM results, and
   what remains.

Open items on other hosts (for a later Mac or Linux run, not this one).
Commit cd623df pushed fixes for the first three but no CI run has
verified them yet; check the next `bundle`, `release` (dry run) and `ci`
runs on master before assuming they are closed:
- release.yml "targets (macOS, real SDK)": `sqlite3_changes64` needs
  macOS 12.3; cd623df sets the bundled runtimes' deployment target to
  12.3 (`MACOS_MIN` in runtime/Makefile) and adds libsqlite3.tbd.
- bundle.yml bundle-macos: `bin/lld` built with Homebrew LLVM depended on
  `libunwind.1.dylib` outside the OS; cd623df links the static archives by
  full path without Homebrew's lib dir.
- cross job on Linux: Windows cross links failed with
  `invalid linker name in argument '-fuse-ld=lld'`; cd623df passes
  `--ld-path=<found lld>` in the clang fallback of crosslink.rs.
- ci macos: tests/debug.rs — lldb finds no debug info (dSYM is written by
  d94f04e; diagnostics in the macOS step show what lldb sees), and Apple's
  lldb lacks `--recognizer-function` (Homebrew lldb is now used, 983770d).
  Still open.
