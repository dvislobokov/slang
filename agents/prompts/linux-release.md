# Prompt: the Linux release (v0.1.0) and the self-contained toolchain, run locally

Run on a Linux x86-64 machine (Ubuntu 24.04 or similar) with 8+ cores and
60+ GB free: `./scripts/setup.sh` (LLVM 18, clang, lld), Rust stable, Go
1.24 (only for benchmarks), Docker (for the clean-container smoke tests),
`gh` logged in with push rights to `zell-lang/zell`, and optionally
`mingw-w64`, `wine64`, `qemu-user-static`, `gcc-aarch64-linux-gnu`,
`musl-tools` for the foreign targets. Start `claude` in the repository
root (`master` at or after commit 7b9102e) and paste the prompt below.
Use Opus subagents in worktrees for independent parts; verify locally
before pushing, the CI runner queue is small (tag commits that need no
run with `[skip ci]`).

Background: Zell's toolchain is self-contained (docs/PLATFORMS.md
"Дистрибутив"): the archive carries `bin/zell`, `zell-lsp`, `zell-mcp`,
`bin/lld` and `targets/<triple>/` with the link inputs of every tier-1/2
target (glibc stubs, musl, mingw-w64, macOS tbd, compiler-rt builtins,
`libzell_rt.a`), so a user needs no clang, libc6-dev or SDK. `bundle.yml`
(build on linux-amd64/arm64, `cross.rs bundle::*` from the archive, a bare
`ubuntu:24.04` container, glibc executables on `ubuntu:20.04`, Linux-cross-
built executables on real macOS and Windows runners) was fully green at
7d7aa41 and the `release.yml` dry run produced all five host archives at
4673455 (thin ones). Since then: cd623df (lld without Homebrew libunwind,
macOS deployment target 12.3 + libsqlite3.tbd, `--ld-path` in the clang
fallback) and 7b9102e (`hosts=linux` input for a Linux-only release) are
pushed but unverified by CI. The toolchain looks for `std/`, `targets/`
and `bin/lld` relative to the real path of the `zell` executable, so it
may live anywhere (`~/.zell/toolchain/...` with symlinks in `~/.local/bin`
is what `install.sh` does; `--dir`/`--bin`, `ZELL_STD`, `ZELL_TARGETS`
override; `zell doctor` checks a layout).

---

Read CLAUDE.md, docs/PLATFORMS.md ("Дистрибутив", "Релизы"),
.github/workflows/{ci.yml,bundle.yml,release.yml}, scripts/
{package-toolchain.sh,bundle-targets.sh,build-lld.sh,bundle-smoke.sh,install.sh},
compiler/src/{crosslink.rs,build.rs,toolchain.rs}, compiler/tests/cross.rs
(`bundle::*`), and the latest runs (`gh run list --limit 20`,
`gh run view <id> --log-failed`). Goal: publish **v0.1.0 for Linux**
(linux-amd64 required, linux-arm64 if it builds) as a self-contained
archive that installs anywhere, and leave the repository in a state where
macOS and Windows archives can be added to the same release later
(`agents/prompts/macos-ci.md`, `windows-ci.md`).

1. **CI on master for Linux.** Get `ci.yml` jobs linux-amd64, linux-arm64
   and cross green (ignore the macOS/Windows jobs here; they have their
   own prompts). Known: the cross job's Windows links failed with
   `-fuse-ld=lld` before cd623df; `spec_stdlib4 postgres_mock` and
   `redis_mock` timed out on arm64 once (load?). `cargo test --workspace`
   must pass locally first.
2. **Bundle on Linux.** `bundle.yml` green at the current master; run the
   same steps locally: `scripts/bundle-targets.sh` (pinned downloads with
   sha256, cached), `scripts/build-lld.sh`, `scripts/package-toolchain.sh`,
   then `scripts/bundle-smoke.sh` in a bare `ubuntu:24.04` container and
   the glibc floor check on `ubuntu:20.04`. Verify the install story
   end to end from the archive: `install.sh --from <archive>` into a
   custom `--dir` under $HOME and a custom `--bin`, `zell doctor`,
   `zell run examples/hello.zl`, `zell build --release`, `zell test` on a
   `zell init` project, `zell build --target` for linux-arm64 (run under
   qemu), linux-amd64-musl (static: `ldd` says so), windows-amd64 (run
   under wine), macos-arm64 (file type), with clang, libc6-dev and lld
   absent from the container. Also: install as a non-root user into
   `~/.local`, and a system-wide install into `/opt/zell` with a symlink
   in `/usr/local/bin` (both must work; document both in README).
   Check the archive size (target ≤ 80 MB; strip, xz) and THIRD_PARTY.md.
3. **release.yml dry run, hosts=linux**: `gh workflow run release.yml -f
   tag=v0.1.0 -f hosts=linux` (dry_run stays true), watch it, fix until
   meta, targets, targets-macos (the macOS link inputs built on a mac
   runner against the real SDK; needed for cross-compiling to macOS from
   Linux), linux-amd64, linux-arm64 and the release job (artifact only)
   are green. If targets-macos cannot be made green quickly, make the
   Linux archives fall back to the stub-SDK-built macOS inputs that
   bundle.yml already produces, and say so in the release notes.
4. **Release notes and docs**: write `CHANGELOG.md` (v0.1.0: what the
   language and toolchain contain, platforms, known gaps: macOS and
   Windows archives pending, the open performance items from
   benchmarks/README.md), update README "Install" for the real download
   URLs (`https://github.com/zell-lang/zell/releases/download/v0.1.0/...`)
   and the one-line installer, and docs/PLATFORMS.md "Релизы".
5. **Tag and publish**: `git tag -a v0.1.0 -m "<notes>"` on the verified
   commit, `git push origin v0.1.0`. The tag push starts release.yml with
   all hosts; if macOS/Windows jobs fail there the release job will not
   create the release, so also run `gh workflow run release.yml -f
   tag=v0.1.0 -f hosts=linux -f dry_run=false`, which creates the GitHub
   Release with the Linux assets. Then verify as a user: on a clean
   machine or container, `curl -fsSL https://raw.githubusercontent.com/zell-lang/zell/master/scripts/install.sh | sh`
   installs v0.1.0 and `zell run` works. Mark the release as pre-release
   if the macOS/Windows assets are still missing.
6. Report: the run URLs, the archive sizes, the exact install commands
   verified, and what remains for macOS and Windows.
