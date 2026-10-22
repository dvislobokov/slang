# Prompt: macOS and the self-contained toolchain, run locally

Run on a Mac (Apple Silicon preferred; an Intel Mac or a second machine
for x86_64 checks is a plus) with: Xcode Command Line Tools, Homebrew
`llvm@18` (`brew install llvm@18 zstd`), Rust stable, Git. Start `claude`
in the repository root (`master` at or after commit ed9cd9a) and paste the
prompt below. Use Opus subagents for independent parts; verify locally
before pushing, the CI runner queue is small.

Background: on 2026-10-22 the first real CI runs on macOS happened
(`ci.yml` jobs macos-arm64 on macos-14 and macos-amd64 on macos-15-intel).
Fixed that day: dyld frees thread-local storage before the runtime's
pthread key destructors run (4fd1b13: per-thread state moved to the heap),
kqueue reported READ and WRITE as two events (febe094), kqueue refusing
/dev/null (89a4503), 8 KB socket buffers and `somaxconn=128` in tests,
Apple ld flags for release/strip (8edbe25: `-dead_strip`, `-S -x`, no
`-static`), dSYM after linking debug builds (d94f04e), Homebrew lldb for
the debugger tests (983770d), `// os: linux` spec directive (bcd0889).
The runtime's 21 C test binaries pass on both macOS runners. The
self-contained bundle (`bundle.yml`: `bin/lld` + `targets/<triple>/` link
inputs, no clang needed) is green when built on Linux; its Linux-cross-built
macOS executables run on the macOS runners. Commit cd623df pushed fixes
for the macOS-host bundle (lld linked without Homebrew's libunwind,
deployment target 12.3 because the SDK marks `sqlite3_changes64` 12.3+,
a libsqlite3.tbd in the bundle) that no CI run has verified yet.

---

Read CLAUDE.md, docs/PLATFORMS.md ("Дистрибутив", "Релизы", the macOS
rows), .github/workflows/{ci.yml,bundle.yml,release.yml},
scripts/{macos-verify.sh,package-toolchain.sh,bundle-targets.sh,build-lld.sh,install.sh},
compiler/src/{crosslink.rs,build.rs,toolchain.rs}, runtime/Makefile,
runtime/src/platform/{posix.c,poll_kqueue.c,prof_*.c}, tools/lldb/,
compiler/tests/{debug.rs,cross.rs} and the latest runs at
https://github.com/zell-lang/zell/actions (`gh run list`, `gh run view
--log-failed`). Goal: macOS is a first-class host: every `ci` macOS step
green on arm64 and amd64, the self-contained archive built on a Mac and
tested on a clean Mac, the release dry run green for macos-arm64 and
macos-amd64, and a clean Mac able to unpack the archive and build/run
programs for macOS and cross-compile for Linux and Windows with nothing
else installed (no Xcode, no Command Line Tools, no Homebrew).

1. **`cargo test --workspace` green on macOS.** Run it locally
   (`LLVM_SYS_180_PREFIX=$(brew --prefix llvm@18)`,
   `LIBRARY_PATH=$(brew --prefix)/lib` for zstd, `make -C runtime` first).
   Known open item: tests/debug.rs. On the runner lldb reported
   "Breakpoint: no locations" even after the dSYM step; the macOS CI step
   prints `dwarfdump --uuid` of the executable and its dSYM and an lldb
   `image list` before the tests (a390fc9), read that output in the last
   macos-arm64 run. Likely causes: the dSYM is written next to the output
   but the test copies or renames the executable, the UUIDs differ because
   the object is relinked, or `-dead_strip`/strip removes the debug map;
   also check that the generated DWARF has `DW_AT_name` and line tables
   lldb accepts (compare with clang's output for a C file). Then the
   formatters: `tools/lldb/zell_formatters.py` uses
   `--recognizer-function`, which Apple's lldb (1500) rejects; with
   Homebrew lldb 18 it should work, make the tests find that lldb (PATH
   order, `ZELL_LLDB`), and make the formatters degrade gracefully on an
   lldb without recognizer support (type summaries by name) instead of
   failing. The DAP-based `zell debug` must work with `lldb-dap` from
   Homebrew too (`zell debug run`, VS Code launch config).
   Fix everything else that fails at the root (platform code only in
   runtime/src/platform/, decisions recorded in examples/README.md or
   tests/spec/README.md; use `// os: linux` only for genuinely Linux-only
   specs). Make `scripts/macos-verify.sh` (kqueue, sampler, symbols, tz,
   roots, processes, packaging) pass fully, not only `--quick`.
2. **Bundle built on the Mac itself.** Verify cd623df: `bin/lld` must
   depend only on /usr/lib and /System (`otool -L`), the macOS runtimes
   build against the real SDK with deployment target 12.3 for arm64 and
   x86_64, `libSystem.tbd`/`libsqlite3.tbd` match the SDK, compiler-rt
   builtins for both arches are present. Then
   `scripts/package-toolchain.sh` on the Mac produces
   `zell-<version>-macos-arm64.tar.gz` (and amd64 on an Intel Mac or
   cross: decide and document) with `targets/` for every tier-1/2 target
   so cross-compiling to linux-amd64/arm64, musl and windows-amd64 works
   from a Mac; `zell doctor` passes from the unpacked archive. Code
   signing/notarization: at least ad-hoc sign the binaries
   (`codesign -s -`) so Gatekeeper and arm64 kernel accept them; document
   the quarantine attribute (`xattr -d com.apple.quarantine`) for
   downloads, and what a notarized release would need later.
3. **Test the archive on a clean Mac** (a fresh user account is not
   enough: use a VM, e.g. UTM/Tart with a clean macOS image, or at least
   a machine without Xcode CLT and Homebrew): `install.sh --from
   <archive>`, `zell run examples/hello.zl`, `zell build --release
   examples/http_server.zl`, `zell test` on a `zell init` project,
   `zell build --target linux-amd64` / `windows-amd64` of hello.zl (check
   file types; run the Linux binary in Docker if available), the VS Code
   extension against `zell-lsp`. Script it as scripts/bundle-smoke-macos.sh
   and extend `bundle.yml`'s bundle-macos jobs to build the bundle on the
   macOS runners and run that script (the runner is not clean: at least
   run with Homebrew and the CLT removed from PATH and `SDKROOT` unset).
4. **release.yml**: the macos-arm64 and macos-amd64 jobs must produce the
   self-contained archives; run the dry run (`gh workflow run release.yml
   -f tag=v0.1.0`) and get both macOS jobs and the "targets (macOS, real
   SDK)" job green.
5. **`ci.yml` macOS jobs**: fast and honest (cache Homebrew LLVM and
   cargo), the full `macos-verify.sh`, the lldb tests enabled.
6. Push to `master` in logical commits (English imperative, `[skip ci]`
   for commits that need no run; batch related changes) and watch the
   runs with `gh`. Report: every macOS failure and its root cause, the
   archive layout and size, the clean-Mac results, what a notarized
   release needs, and what remains.

Open items on other hosts (not this one): see `windows-ci.md` for
Windows; the Linux cross job's `-fuse-ld=lld` regression was fixed in
cd623df (verify in the next `ci` run on master).
