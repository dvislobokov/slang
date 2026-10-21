# CLAUDE.md — guide for AI agents working on Slang

Slang is a compiled language (Go-like simplicity, C#-style namespaces and
`var` inference, methods inside structs, `T!` errors with `?`/`try`,
`async`/`await`, no GC) implemented as a Rust compiler on LLVM 18 with a C
runtime. Read `DESIGN.md` before touching anything: it is the language spec
and every change must match it or update it.

## Layout

| Path | What | Owner docs |
|---|---|---|
| `DESIGN.md` | language spec (syntax, semantics, memory, errors, async, reflection, stdlib) | — |
| `docs/TOOLING.md`, `PACKAGES.md`, `FRAMEWORK.md`, `PLATFORMS.md` | tooling/LSP, package manager, web/ORM/log/diag framework, platforms | — |
| `compiler/` | Rust crate `slang`: `lexer` → `parser` (`ast.rs`) → `sema/` (resolve, check, lower to `hir.rs`) → `codegen/` (inkwell/LLVM) → `build.rs` (object + link); `package/` (manifest, graph, lock); `session.rs` + `sema/index.rs` (LSP queries); `main.rs` CLI | `codegen/mod.rs` header = compiler-side ABI |
| `runtime/` | C11 runtime: refcounted objects, strings, slices, maps, errors, panic, I/O, task scheduler (`task.c`), stdlib `std_*.c` | `runtime/ABI.md` = C-side ABI, exact |
| `lsp/` | `slang-lsp` binary (lsp-server) | `docs/TOOLING.md` |
| `editors/vscode/` | TextMate grammar + LSP client | — |
| `grammar/slang.bnf` | canonical Grammar-Kit grammar (IDEA plugin); keep in sync with parser and `DESIGN.md` EBNF | — |
| `docs/guide/` | user guides in English (tour, errors, async, memory, packages, testing, debugging, web, faq); every ```slang block is compiled and run by `compiler/tests/guide.rs` | — |
| `examples/` | runnable `.sl` + byte-exact `.out`; `errors_compile/` must fail; `README.md` lists output rules and assumptions | — |
| `tests/spec/` | spec corpus, `// status: passing|pending` per file (TDD: pending tests must fail; flip when implemented) | `tests/spec/README.md` |
| `benchmarks/` | Slang vs Go programs; `scripts/bench-perf.sh`, `scripts/bench-size.sh` | `benchmarks/README.md` |

## Build and test

```sh
./scripts/setup.sh                 # once: llvm-18-dev, clang, lld (Ubuntu)
make -C runtime && make -C runtime test     # C runtime (-Werror), `make asan` for sanitizers
make -C runtime CROSS=x86_64-w64-mingw32 test-wine  # Windows runtime (mingw-w64, -Werror) + C tests under wine
make -C runtime CROSS=aarch64-linux-gnu test-cross  # linux-arm64 runtime + C tests under qemu (also CROSS=x86_64-linux-musl)
make -C runtime CROSS=aarch64-apple-darwin          # macOS: compile check against runtime/stub-sdk/macos (no SDK here)
cd compiler && cargo build && cargo test    # unit, parser, sema, package, run_examples, spec
cargo build --release && ./target/release/slang run examples/hello.sl
./target/release/slang check --dump-hir file.sl   # inspect typed HIR
./target/release/slang build file.sl --emit=llvm-ir # inspect IR
```

`cargo test` builds and runs every example byte-exact against its `.out`,
so the runtime must be built first (`make -C runtime`). Keep `cargo build`
warning-free and the runtime `-Werror` clean.

## Rules

- **Spec first.** New syntax or semantics go into `DESIGN.md` (and
  `grammar/slang.bnf` + the EBNF section) in the same change. Examples and
  spec tests are the acceptance criteria: add a `.sl` + `.out` for every
  feature, and a `// error:` program for every new diagnostic.
- **ABI is a contract.** Anything crossing compiler↔runtime is documented in
  `runtime/ABI.md` (C side) and `compiler/src/codegen/mod.rs` (compiler
  side): layouts, ownership (+1 returns, borrowed args), NULL rules. Change
  both or neither. Stdlib externs are declared in `compiler/src/sema/stdlib.rs`
  and implemented as `slang_<ns>_<name>` in `runtime/src/std_*.c`.
- **Output format** follows `examples/README.md`: `println` separates with
  one space; floats print like Go `%v` (fixed unless exp < -4 or >= 6);
  slices print `[1 2 3]`; errors print `Message()`.
- **Memory**: no GC. ARC per `DESIGN.md` "Память"; never introduce a
  tracing collector. Double frees are bugs; leaks are tracked by
  `slang run --leak-check` and must not grow.
- **Go semantics are the default** wherever `DESIGN.md` is silent
  (integer division, terminating statements, defer, switch, maps), then
  record the decision in `examples/README.md` or `tests/spec/README.md`.
- **Cross-platform**: platform-specific runtime code belongs in
  `runtime/src/platform/`; no `#ifdef _WIN32` elsewhere (see
  `docs/PLATFORMS.md`).
- **Performance** regressions on `scripts/bench-perf.sh` need a reason in
  the commit message; sizes from `scripts/bench-size.sh` are tracked in
  `benchmarks/README.md`.
- **Git**: commit on `master`, small focused commits, message in English
  imperative ("Add X", "Fix Y"); never commit `target/`, `build/`,
  `*.o`, `libslang_rt.a`, `node_modules/`.
- **Parallel agents**: when several agents work at once, each owns disjoint
  paths; do not edit another agent's files, do not commit for them, and if
  the tree is temporarily broken by in-progress edits, wait and retry
  instead of "fixing" their code.
- Code style: Rust idiomatic, modules under ~1500 lines, doc comments on
  public items; C11 with `-Wall -Wextra -Werror`, every symbol prefixed
  `slang_` (private `slang__`).

## Current status and roadmap

Done: lexer/parser, sema + HIR, LLVM codegen (structs, interfaces, enums,
closures, defer, errors, async via LLVM coroutines), C runtime + core
stdlib, `slang build/run/check`, package manifests/workspaces/lock,
LSP + VS Code client, examples and benchmarks.

Next (see `DESIGN.md` stages and `tests/spec` pending counts): memory
management (`Drop`, `weak`, ownership elision), generics, block `try`,
error traces, reflection + attributes as values, `slang fmt`, stdlib
(`collections`, `net`, `http`, `encoding.json`), framework packages
(`docs/FRAMEWORK.md`), platforms (`docs/PLATFORMS.md`).
