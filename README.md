# Slang

Slang is a compiled, statically typed language for services, tools and
command-line programs. It keeps Go's small and readable core and adds
what Go makes you write by hand:

- **C#-style namespaces and `var`** inference, **methods inside structs**;
- **errors as values without the boilerplate**: `T!` results, `?` to
  propagate, `try ... catch` / `try ... else` to handle, and an error
  trace of where each error was created and passed through;
- **`async`/`await`** on a multi-threaded task scheduler, with channels,
  structured cancellation and task-local values;
- **generics** (monomorphized), enums with data, exhaustive `switch`,
  attributes and compile-time reflection (JSON, SQL mapping and DI cost
  nothing at run time);
- **no garbage collector**: reference counting with compile-time
  ownership, destructors (`Drop`) and `weak` pointers, so there are no GC
  pauses and resources close deterministically;
- a large standard library written mostly in Slang (HTTP/1.1 and 2, TLS
  1.3, SQL with PostgreSQL and SQLite drivers, JSON/YAML/TOML/XML, regex,
  crypto, compression, structured logging, metrics and tracing, a web
  framework), compiled through LLVM 18 to native code.

```slang
namespace hello

import std
import std.io

struct User {
    Name string
    Age  int
}

func parse(line string) User! {
    var parts = strings.Split(line, ":")
    if len(parts) != 2 {
        return error("expected name:age, got " + strconv.Quote(line))
    }
    return User{Name: parts[0], Age: strings.ToInt(parts[1])?("age of " + parts[0])}
}

async func main() {
    for line in ["ann:31", "bob:x", "cid"] {
        var u = try parse(line) catch err {
            println("skipped:", err)
            continue
        }
        await task.Delay(1)
        println(u.Name, "is", u.Age)
    }
}
// Output:
// ann is 31
// skipped: age of bob: invalid integer: "x"
// skipped: expected name:age, got "cid"
```

## Install

**A prebuilt toolchain** (`bin/slang`, `bin/slang-lsp`, `std/`, the
runtime for the machine in `targets/<triple>/`, debugger formatters):

```sh
curl -fsSL https://raw.githubusercontent.com/dvislobokov/slang/master/scripts/install.sh | sh   # Linux, macOS
irm https://raw.githubusercontent.com/dvislobokov/slang/master/scripts/install.ps1 | iex        # Windows (PowerShell)
```

`install.sh` unpacks `slang-<version>-<host>.tar.gz` (from the GitHub
release, or `$SLANG_DIST_URL`, or `--from <archive>`) into
`~/.slang/toolchain/` and links `slang` into `~/.local/bin`; `install.ps1`
does the same under `%USERPROFILE%\.slang\toolchain` and adds `bin\` to
the user `PATH`. Linking programs needs `clang` (and `lld` for cross
targets) on `PATH`. Runtimes for other targets are downloaded on demand:

```sh
slang target list                     # known targets and where their runtime is
slang target add windows-amd64 linux-arm64 linux-amd64-musl   # into ~/.slang/targets
slang build --target windows-amd64 app.sl                     # app.exe (run it with Wine: slang run --target ...)
```

(`$SLANG_TARGETS_URL` overrides the download URL template, see
`docs/PLATFORMS.md`.) A toolchain archive is made with
`scripts/package-toolchain.sh` (plus one `slang-runtime-<version>-<triple>.tar.gz`
per cross-built runtime).

**From source** (Linux x86-64 or arm64, macOS; Windows through MSYS2). You
need Rust, LLVM 18, clang and lld:

```sh
git clone https://github.com/dvislobokov/slang && cd slang
./scripts/setup.sh                    # Ubuntu 24.04: llvm-18-dev, clang-18, lld-18 (run as root)
make -C runtime                       # the C runtime (libslang_rt.a)
cargo build --release                 # target/release/slang and slang-lsp
export PATH="$PWD/target/release:$PATH"
make -C runtime CROSS=x86_64-w64-mingw32   # optional: the Windows runtime (mingw-w64)
make -C runtime CROSS=aarch64-linux-gnu    # optional: linux-arm64 (gcc-aarch64-linux-gnu)
make -C runtime CROSS=x86_64-linux-musl    # optional: static musl binaries (musl-tools)
```

The compiler finds the standard library sources in the repository's
`std/` (or `$SLANG_STD`, or `<prefix>/std` next to an installed
`<prefix>/bin/slang`). Supported platforms and how each was verified:
`docs/PLATFORMS.md`.

## Hello, world

```slang
namespace hello

import std.io

func main() {
    println("Hello, Slang!")
}
// Output:
// Hello, Slang!
```

```sh
slang run hello.sl                    # build and run one file
slang init myapp && cd myapp && slang run    # a project: slang.toml + src/main.sl
slang build --release                 # an optimized, stripped binary
```

## Learn

| | |
|---|---|
| [A tour of Slang](docs/guide/tour.md) | the language in twenty minutes |
| [Errors](docs/guide/errors.md) | `T!`, `?`, `try`, typed errors, wrapping, traces |
| [Async](docs/guide/async.md) | tasks, channels, mutexes, cancellation, timeouts |
| [Memory](docs/guide/memory.md) | ARC, `Drop`, `weak`, arenas, interior pointers |
| [Packages](docs/guide/packages.md) | projects, dependencies, workspaces |
| [Testing](docs/guide/testing.md) | `slang test`, benchmarks, runnable examples |
| [Debugging](docs/guide/debugging.md) | traces, logging, leak checks, the debugger, profiles |
| [Web services](docs/guide/web.md) | `std.web` quickstart |
| [FAQ](docs/guide/faq.md) | differences from Go and C# |
| API reference | `slang doc strings.Split`, or `slang doc std --html out/` for a searchable site |

Every code block in these guides is compiled and run by the test suite
(`compiler/tests/guide.rs`). The language specification is
[DESIGN.md](DESIGN.md) (in Russian), with [docs/TOOLING.md](docs/TOOLING.md),
[docs/PACKAGES.md](docs/PACKAGES.md), [docs/FRAMEWORK.md](docs/FRAMEWORK.md),
[docs/STDLIB.md](docs/STDLIB.md) and [docs/PLATFORMS.md](docs/PLATFORMS.md).
API documentation is written in English, in the comments above the
declarations of `std/`.

### The documentation site

`slang doc --site site` builds the whole site from the repository: this
README as the landing page, the guides, the design documents under
`design/` and the standard library reference under `std/`, with one
search index and relative links. `scripts/docs-serve.sh` builds it and
serves it at `http://localhost:8000/`.

The workflow `.github/workflows/docs.yml` publishes it to GitHub Pages on
every push to `master` (`slang doc --site site --base /<repo>/`). To
enable it once: **Settings → Pages → Source: GitHub Actions**; the site
then lives at `https://<user>.github.io/<repo>/`.

## Status

Slang is young: the language is usable and heavily tested, but not yet
stable, and there is no binary release.

| Area | Status |
|---|---|
| Language core: namespaces, structs, interfaces (default methods, `sealed`), enums with data, closures, `defer`, `switch`, operator overloading, iteration protocol | works |
| Generics (functions, structs, enums, methods; constraints) | works |
| Errors: `T!`, `?`, `try` (expression and block), traces | works |
| `async`/`await`, scheduler, channels, cancellation, task context | works |
| Memory: ARC with ownership, `Drop`, `weak`, arenas, interior-pointer rules, `--leak-check` | works |
| Attributes and reflection (compile-time and run-time) | works |
| Standard library | most of Go's standard library has a counterpart; see [docs/STDLIB.md](docs/STDLIB.md) for each package |
| Networking: TCP/UDP, HTTP/1.1 + HTTP/2 client and server, WebSocket, TLS 1.2/1.3 | works |
| Data: SQL (PostgreSQL, SQLite), Redis, Kafka, S3 clients | works |
| `std.web` framework (routing, binding, validation, DI, auth, OpenAPI) | works |
| Tools: `fmt`, `test` (with examples), `doc`, LSP + VS Code, debugger (in-process agent, DAP, lldb/gdb) | works |
| Packages: manifests, workspaces, git modules with MVS, lock file, module proxy | works |
| Platforms: Linux x86-64/arm64 | works; macOS and Windows (mingw-w64 cross builds) in progress |
| FFI, a public package index, binary releases | not yet |

The test suite (`cargo test --workspace`) builds and runs 47 example
programs byte-exact, a specification corpus of 850+ programs, the
standard library's own examples (`slang test std`) and the guides.

## Performance

Benchmarks against equivalent Go programs (`benchmarks/`, `--release`,
x86-64, wall time, best of 5; Go 1.24):

| fib | map | nbody | sieve | strings | json_like | struct_map |
|---|---|---|---|---|---|---|
| 0.57x | 0.56x | 0.97x | 0.61x | 0.80x | 0.62x | 0.86x |

(Slang time / Go time: below 1 is faster.) A TCP echo of 2 GB through
1000 connections runs at about 1.9 GB/s against Go's 1.6 GB/s, and the
`std.net.http` server handles about the same keep-alive load as Go's
`net/http`. Details and methodology: [benchmarks/README.md](benchmarks/README.md).

## Command line

```
slang run   [path] [options] [-- args]   # build and run (a file, a directory, or the project)
slang build [path] [options]             # compile (default output: ./<name>, or target/<profile>/<pkg>)
slang check [path] [--dump-hir]          # type-check only
slang test  [path] [-v] [--filter re] [--json] [--bench] [--parallel N]   # tests, benchmarks, examples
slang doc   [path|namespace|symbol] [--html dir] [--json] [--check]      # documentation
slang fmt   [paths] [--check] [--diff] [--stdin]   # format in the canonical style
slang init  [dir] [--name n] [--format toml|json|yaml] [--lib]
slang install | update | tree | why <module> | vendor   # dependencies
slang store verify|prune|path | env [NAME] | proxy serve --dir <cache>
slang lsp                                # language server on stdio
slang debug run [path] | debug attach [host:]port | debug [--lldb|--gdb] [path]
slang tool pprof [args] <profile>        # inspect a std.diag CPU profile
slang target list | add <target>...      # prebuilt runtimes for cross-compilation
```

`path` is a file (one program), a directory without a manifest (all its
`.sl` files form one program) or a project (the nearest `slang.toml`,
`slang.json` or `slang.yaml`; a `[workspace]` root builds its members).

| Build option | Effect |
|---|---|
| `-o <path>` | output file |
| `--release` | `-O2`, `--gc-sections`, stripped, no debug info |
| `-g` / `--strip` | keep DWARF in a release build / no debug info at all |
| `--debug-agent` / `--no-debug-agent` | the in-process debugger hooks (default: on in debug builds) |
| `-O0` … `-O3`, `--static` | optimisation level, fully static executable |
| `--target <triple\|alias>` | cross-compile (`windows-amd64`, `linux-arm64`, `linux-amd64-musl`, `macos-arm64`, ...) |
| `--emit=exe\|obj\|llvm-ir` | what to produce |
| `--leak-check` | report objects still alive at exit (exit status 66) |
| `--workspace`, `-p <name>` | all members / one member of a workspace |
| `--locked`, `--offline` | fail if `slang.lock` would change / use only the lock and the store |

## Repository layout

| Path | What |
|---|---|
| `compiler/` | the `slang` compiler (Rust, inkwell/LLVM 18): lexer, parser, semantic analysis, codegen, build, packages, `doc`, `fmt`, `test` |
| `runtime/` | the C runtime: allocator, refcounting, strings, slices, maps, scheduler, I/O, the C parts of the stdlib (`runtime/ABI.md`) |
| `std/` | the standard library sources (written by `compiler/build.rs` from `compiler/src/sema/stdlib_src/`), plus its tests and examples (`*_test.sl`) |
| `lsp/`, `editors/vscode/`, `grammar/` | language server, VS Code extension, Grammar-Kit grammar |
| `examples/` | runnable programs with byte-exact expected output |
| `tests/spec/` | the specification corpus |
| `benchmarks/` | Slang vs Go programs and scripts |
| `docs/` | design documents and the user guides (`docs/guide/`) |

Contributors (human or AI) should read [CLAUDE.md](CLAUDE.md) first: it
describes the architecture, the rules (spec first, ABI contracts, output
format) and how to build and test.

Programs link only the Slang runtime, libc, libm and pthreads, plus a
system library when a module needs one (`std.database.sqlite` links
`-lsqlite3`). TLS, crypto, compression and the PostgreSQL driver are
self-contained.
