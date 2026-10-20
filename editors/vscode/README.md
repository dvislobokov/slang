# Slang for VS Code

Language support for `.sl` files:

- **Syntax highlighting** without a server: the TextMate grammar in
  `syntaxes/slang.tmLanguage.json`.
- **Language server** (`slang-lsp`, the `lsp/` crate of this repository):
  diagnostics as you type (also in half-written code: the parser recovers),
  quick fixes (unused variable, unhandled `T!`, unused or missing import),
  document outline, go to definition, find references, highlights, hover
  (signature + doc comment), rename across files, semantic highlighting,
  folding, completion (members after `.`, names in scope, keywords),
  signature help, inlay hints (inferred `var` types, parameter names),
  formatting (`slang fmt` rules, whole document or selection) and
  workspace symbols.

## Build the language server

From the repository root:

```sh
cargo build --release -p slang-lsp     # target/release/slang-lsp
```

Put `target/release` on `PATH`, or point the `slang.serverPath` setting at
the binary. If `slang-lsp` is not on `PATH` but the `slang` CLI is, the
extension runs `slang lsp` instead (which starts the `slang-lsp` binary
installed next to `slang`).

## Build and run the extension

```sh
cd editors/vscode
npm install
npm run build        # type-checks with tsc and bundles to out/extension.js
```

Then open `editors/vscode` in VS Code and press **F5** ("Run Slang
extension"): an Extension Development Host opens on the repository's
`examples/` folder. Open any `.sl` file to start the server; its log is in
the **Output** panel under "Slang" (set `"slang.trace.server": "verbose"` to
see the LSP traffic).

`npm run watch` rebuilds the bundle on every change. To install the
extension permanently, package it with `npx @vscode/vsce package` and install
the `.vsix` (Extensions view, "Install from VSIX...").

## Settings and commands

| Setting | Default | Meaning |
|---------|---------|---------|
| `slang.serverPath` | `slang-lsp` | Server executable (a path to `slang` runs `slang lsp`) |
| `slang.serverArgs` | `[]` | Extra server arguments |
| `slang.trace.server` | `off` | `messages` / `verbose` LSP tracing |
| `slang.inlayHints.types.enabled` | `true` | inferred type after `var x = ...` / `for x in ...` names |
| `slang.inlayHints.parameterNames.enabled` | `true` | parameter names before literal call arguments |
| `slang.path` | `slang` | CLI used to build for debugging and to find the formatters |
| `slang.debug.lldbDapPath` | `""` | `lldb-dap` executable (default: `lldb-dap`, `lldb-dap-18`, ... on `PATH`) |
| `slang.debug.formatters` | `""` | path of `slang_formatters.py` (default: `slang debug --formatters lldb`) |

Command palette: **Slang: Restart server** (also done automatically when
`slang.serverPath` changes) and **Slang: Format document (slang fmt)**.

### Formatting

The server formats with the canonical `slang fmt` style (the same code as
the CLI). The extension makes itself the default formatter for `.sl` files
and turns on format-on-save for them (`configurationDefaults`); to opt out,
add to your settings:

```json
"[slang]": { "editor.formatOnSave": false }
```

**Format Selection** formats only the changed lines inside the selection.
A file with syntax errors is left unchanged.

## Debugging

The extension contributes the debug type **`slang`**. It runs
[`lldb-dap`](https://lldb.llvm.org/use/lldbdap.html) (LLDB 18+; install
`lldb-18` on Ubuntu, or point `slang.debug.lldbDapPath` at it) with the
Slang formatters (`tools/lldb/slang_formatters.py`, located with
`slang debug --formatters lldb`) loaded through `initCommands`, so strings,
slices, maps, errors, `T!`, enums, interfaces, closures and tasks show as
Slang values. Breakpoints, stepping, the call stack, variables, watch and
the debug console (lldb commands; expressions in C syntax) work.

F5 on a `.sl` file without a `launch.json` builds it (`slang build`; debug
info is on by default) and debugs it. A `launch.json`:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "slang",
      "request": "launch",
      "name": "Debug Slang file",
      "source": "${file}",
      "program": "${workspaceFolder}/target/slang-debug/${fileBasenameNoExtension}",
      "args": [],
      "env": { "SLANG_WORKERS": "2" },
      "cwd": "${workspaceFolder}",
      "preLaunchTask": "slang: build"
    },
    { "type": "slang", "request": "attach", "name": "Attach", "pid": "${command:pickProcess}" }
  ]
}
```

- `launch`: `program` (executable), or `source` (file / project) which is
  built first unless a `preLaunchTask` is set (`build: false` skips it,
  `buildArgs` adds flags, e.g. `["--release", "-g"]`); `args`, `env`
  (object or `["K=V"]`), `cwd`, `stopOnEntry`, and lldb-dap's
  `initCommands`, `preRunCommands`, `stopCommands`.
- `attach`: `pid` (asked for when missing), optionally `program`.
- Task type `slang` (`"command": "build"`, `source`, `program`, `args`):
  the provided task **slang: build** runs
  `slang build ${file} -o target/slang-debug/<name>`.
- In the debug console, `slang-next` steps over a statement and follows an
  `await` to the coroutine's next line (plain `next` stops in the caller
  when the `await` suspends); a panic stops at `slang_panic`.

## Project discovery

On start the server loads every `.sl` file of each workspace folder. If the
folder has a manifest (`slang.toml`, `slang.json` or `slang.yaml`) and a
`src/` directory, only `src/**` is loaded. A file opened outside the
workspace loads the project whose manifest is found walking up from it.
