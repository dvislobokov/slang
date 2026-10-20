# Slang

A compiled language with Go-like simplicity, C#-style namespaces and
`var` type inference, methods inside structs, `T!` error types instead of
`if err != nil`, and built-in `async`/`await`. Backend: LLVM.

See [DESIGN.md](DESIGN.md) for the language design.

## Layout

- `compiler/` — the `slang` compiler (Rust + inkwell/LLVM 18)
- `runtime/` — runtime library in C (allocation, strings, slices, maps, tasks)
- `examples/` — sample `.sl` programs with expected output
- `scripts/setup.sh` — installs build dependencies on Ubuntu

## Build

```
./scripts/setup.sh      # once
cargo build --release
./target/release/slang parse examples/hello.sl
```

## Command line

```
slang build [path] [options]        # compile (default output: ./<name>, or target/<profile>/<pkg>)
slang run   [path] [options] [-- args]
slang check [path] [--dump-hir] [options]
slang test  [path] [options]        # type-checks with dev-dependencies; no test runner yet
slang init  [dir] [--name n] [--format toml|json|yaml] [--lib]
slang manifest schema               # JSON Schema of the manifest
slang manifest convert <toml|json|yaml> [dir]
slang parse <file.sl>               # print the AST
slang fmt   [paths] [--check] [--diff] [--stdin]   # format .sl files (canonical style)
slang lsp                           # language server on stdio
slang debug [--lldb|--gdb] [path] [options] [-- args]   # build with DWARF, start the debugger
```

`path` selects what to build:

- a **file** — a single-file program;
- a **directory without a manifest** — every `.sl` file under it is one
  program (e.g. `slang run examples/multi`);
- a **directory with a manifest**, or nothing — a package project: the
  nearest `slang.toml` / `slang.json` / `slang.yaml` (exactly one per
  directory) is found by walking up from the current directory, and an
  enclosing `[workspace]` that lists it as a member becomes the root.

Build options:

| Option | Effect |
|---|---|
| `-o <path>` | output file (one package) |
| `--release` | `-O2`, linked with `--gc-sections` and stripped (`-s`), no `-rdynamic`, no debug info |
| `-g` | keep DWARF debug info (and symbols) in a `--release` build; debug builds always have it |
| `--strip` | no debug info, stripped executable |
| `-O0` … `-O3` | optimisation level |
| `--static` | fully static executable (`clang -static`) |
| `--emit=exe\|obj\|llvm-ir` | what to produce (`obj`/`llvm-ir`: whole program, one file) |
| `--workspace` | every workspace member |
| `-p, --package <name>` | one member (repeatable) |
| `--locked` | fail if `slang.lock` would change |
| `-v, --verbose` | log the object cache and the link step to stderr |

### Debugging

Debug builds carry DWARF 5 (types, locals, async frames, line tables; see
`docs/DEBUGGER.md`). `slang debug` builds and starts lldb with the Slang
formatters loaded, so values print in Slang terms:

```
$ slang debug --lldb app.sl          # or --gdb; -- args are the program's
(lldb) b app.sl:12
(lldb) run
(lldb) v
(string) name = "slang"
([]int) nums = [5, 3, 8]
(map[string]int) ages = {"alice": 30, "bob": 31}
(app.Result) r = Ok(value = 1)
(app.Shape) s = app.Circle (R = 2)
(lldb) slang-next                    # `next` that follows an `await` to the next line
```

With a plain lldb or gdb, load the formatters by hand:
`command script import tools/lldb/slang_formatters.py` (lldb) or
`source tools/gdb/slang_printers.py` (gdb); `slang debug --formatters lldb`
prints the path. VS Code: debug type `slang` (lldb-dap), see
`editors/vscode/README.md`.

### Formatting

`slang fmt` rewrites `.sl` files in the one canonical style (like `gofmt`,
no options): 4-space indentation, braces on the same line, spaces around
binary operators, struct fields aligned in columns, imports sorted (`std`
first, a blank line before the others), at most one blank line between
statements, comments kept, lines up to 100 columns with long calls,
literals and binary expressions broken one item per line (trailing commas).

```
slang fmt                      # every .sl file under the current directory
slang fmt src/ main.sl         # files and directories
slang fmt --check .            # CI: list unformatted files, exit 1 if any
slang fmt --diff main.sl       # show what would change
slang fmt --stdin < a.sl       # filter (editors)
```

Files with syntax errors are reported and left unchanged. The formatter
re-parses its output and refuses to write anything whose syntax tree
differs from the input. Editors get the same formatting through the
language server (`textDocument/formatting`, see `editors/vscode`).

### Packages

```
$ slang init hello && cd hello && slang run
Hello, world!
```

A package is a directory with a manifest and `src/**/*.sl`. Its `namespace`
(default: the name with `-` → `_`) is its root: all its files declare
`namespace <root>` or `namespace <root>.<sub>`, and no two packages in a
graph share a root. `entry` makes it a binary; without `entry` it is a
library.

```toml
[package]
name = "app"
version = "0.1.0"
namespace = "app"
entry = "src/main.sl"

[dependencies]
lib-a = { path = "../lib-a" }
```

Dependencies are local `path`s for now (registry and git sources come in
later stages, see [docs/PACKAGES.md](docs/PACKAGES.md)). Isolation is
strict: code may `import` (or name by full path) only `std`, its own
package and its **declared** dependencies; using a transitive dependency
is an error that names the package to add. A root manifest with
`[workspace] members = ["app", "libs/*"]` builds members together
(`--workspace`, `-p name`); `target/` and `slang.lock` live next to it.

`slang.lock` lists every package of the graph (name, version, `path:`
source, dependencies, sha256 of its sources) as sorted YAML. It is
rewritten when the graph or sources change; `--locked` turns that into an
error (for CI).

Each package compiles to its own object file,
`target/slang/<pkg>-<hash>.o`; the hash covers the package's IR, the
flags and the compiler, so unchanged packages are reused (`--verbose`
prints `cache hit: <pkg>`). Executables go to `target/debug/<pkg>` or
`target/release/<pkg>`.
