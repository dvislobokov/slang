# Slang for VS Code

Language support for `.sl` files:

- **Syntax highlighting** without a server: the TextMate grammar in
  `syntaxes/slang.tmLanguage.json`.
- **Language server** (`slang-lsp`, the `lsp/` crate of this repository):
  diagnostics as you type, document outline, go to definition, find
  references, hover (signature + doc comment), rename across files,
  semantic highlighting, folding and completion (members after `.`, names
  in scope, keywords).

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

Command palette: **Slang: Restart server** (also done automatically when
`slang.serverPath` changes).

## Project discovery

On start the server loads every `.sl` file of each workspace folder. If the
folder has a manifest (`slang.toml`, `slang.json` or `slang.yaml`) and a
`src/` directory, only `src/**` is loaded. A file opened outside the
workspace loads the project whose manifest is found walking up from it.
