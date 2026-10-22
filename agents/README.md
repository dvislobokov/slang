# Zell for AI coding agents

Language intelligence for Zell in AI coding agents: Claude Code, OpenAI
Codex CLI, Gemini CLI, Cursor, Windsurf, Cline, Aider, Zed, GitHub
Copilot. This directory has everything they need:

| Path | What |
|---|---|
| [`claude-code/`](claude-code) | Claude Code plugin: `zell-lsp` as its language server for `.zl`, `zell-mcp` as its MCP server, a post-edit `zell fmt` + `zell check` hook, the `zell` and `zell-packages` skills |
| [`.claude-plugin/marketplace.json`](.claude-plugin/marketplace.json) | a marketplace with that plugin (for when this directory is its own repository) |
| [`mcp/`](mcp) | `zell-mcp`, the MCP server (Rust, stdio) every MCP-capable agent can use |
| [`templates/AGENTS.md`](templates/AGENTS.md) | the agent guide (both skills, without frontmatter) for `AGENTS.md`, `CONVENTIONS.md`, rules files |
| [`codex/`](codex), [`gemini/`](gemini), [`cursor/`](cursor), [`windsurf/`](windsurf), [`cline/`](cline), [`zed/`](zed), [`aider/`](aider), [`copilot/`](copilot) | per-agent configuration snippets |

The quickest setup for a project is the compiler itself:

```sh
zell init --agents claude,codex,gemini,cursor   # or: all; in an existing package only the agent files are written
```

It writes the guide as each agent's instruction file (a marked section
in an existing `AGENTS.md`/`GEMINI.md`/`CONVENTIONS.md`, refreshed on the
next run), registers `zell-mcp` in the project's MCP configuration
(merging into existing JSON/TOML), and prints the steps that live in user
configuration (Windsurf, Cline). Agents: `claude` (the plugin via
`.claude/settings.json`), `claude-skills` (skills in `.claude/skills/` +
`.mcp.json`, no plugin), `codex`, `gemini`, `cursor`, `windsurf`, `cline`,
`aider`, `zed`, `copilot`.

All of it needs the toolchain on `PATH`: `zell`, `zell-lsp` and `zell-mcp`
(installed by `scripts/install.sh` of the zell repository, or
`cargo build --release` in it: `target/release/`).

## What each agent supports

| Agent | Language server (LSP) | MCP | Instructions | Set up with |
|---|---|---|---|---|
| **Claude Code** | yes, natively, through plugins (`lspServers`): diagnostics after every edit and an `LSP` tool (definition, references, hover, symbols); not in cloud sessions | yes (`.mcp.json`, plugins) | skills, `CLAUDE.md` | the plugin (`claude-code/`) |
| **Codex CLI** | no built-in LSP client (MCP bridges only) | yes, `[mcp_servers.<name>]` in `~/.codex/config.toml` or `.codex/config.toml` (trusted projects), `codex mcp add` | `AGENTS.md` (repo root down to cwd; 32 KiB default cap) | `codex/config.toml` + `templates/AGENTS.md` |
| **Gemini CLI** | no | yes, `mcpServers` in `~/.gemini/settings.json` / `.gemini/settings.json`; extensions (`gemini-extension.json`) | `GEMINI.md` (`context.fileName` can add `AGENTS.md`) | `gemini/` (an extension) or `gemini/settings.json` |
| **Cursor** | through the editor: install the Zell VS Code extension (`editors/vscode`, as a VSIX), the agent sees its diagnostics | yes, `.cursor/mcp.json` / `~/.cursor/mcp.json` | `.cursor/rules/*.mdc`, `AGENTS.md` | `cursor/mcp.json` + `zell init --agents cursor` |
| **Windsurf** | through the editor (VS Code extension) | yes, user-level `~/.codeium/windsurf/mcp_config.json` | `.windsurf/rules/*.md` | `windsurf/mcp_config.json` + `zell init --agents windsurf` |
| **Cline** | through VS Code (the extension's diagnostics) | yes, user-level `cline_mcp_settings.json` | `.clinerules/` | `cline/cline_mcp_settings.json` + `zell init --agents cline` |
| **Aider** | no (tree-sitter repo map; Zell has no grammar there) | not officially documented (unverified third-party reports) | `--read CONVENTIONS.md`, `test-cmd` | `aider/.aider.conf.yml` + `zell init --agents aider` |
| **Zed** | yes, but only through a Zed extension (none for Zell yet) | yes, `context_servers` in settings | `AGENTS.md`, `.rules` | `zed/settings.json` + `zell init --agents zed` |
| **GitHub Copilot (VS Code)** | through VS Code (the Zell extension) | yes, `.vscode/mcp.json` (`servers`) | `AGENTS.md`, `.github/copilot-instructions.md` | `copilot/mcp.json` + `zell init --agents copilot` |

So Claude Code gets both channels: the LSP (diagnostics pushed after
edits, symbol navigation) and the MCP tools (which add rename, format,
tests, builds, std search and docs). Everyone else gets Zell through
`zell-mcp`, plus editor diagnostics where the agent lives in VS Code.

## Claude Code

The plugin (`claude-code/`):

- `.claude-plugin/plugin.json` declares `lspServers.zell` (`zell-lsp`,
  `extensionToLanguage: {".zl": "zell"}`, push diagnostics) and
  `mcpServers.zell` (`zell-mcp`).
- `hooks/hooks.json`: after `Write`/`Edit`/`MultiEdit` of a `.zl` file,
  `scripts/zell-post-edit.sh` runs `zell fmt` on it and `zell check` on its
  project; a failed check exits 2, so Claude sees the errors at once.
  `ZELL_AGENT_FORMAT=0` / `ZELL_AGENT_CHECK=0` turn the steps off.
- `skills/zell/SKILL.md`: the language for agents (syntax, imports without
  `std`, `T!`/`?`/`try`, async, memory, `@test`, CLI, output format).
  `skills/zell-packages/SKILL.md`: manifests, module paths, the package
  commands.

Install it from the zell repository, whose root has a
`.claude-plugin/marketplace.json` pointing at `agents/claude-code`:

```sh
claude plugin marketplace add zell-lang/zell
claude plugin install zell@zell
# inside Claude Code: /plugin marketplace add zell-lang/zell, then /plugin install zell@zell
```

Once this directory moves to its own repository (`zell-lang/zell-agents`),
its own `.claude-plugin/marketplace.json` serves the same plugin:

```sh
claude plugin marketplace add zell-lang/zell-agents
claude plugin install zell@zell
```

For a team, `zell init --agents claude` writes `.claude/settings.json`
with `extraKnownMarketplaces.zell` and `enabledPlugins["zell@zell"]`, so
Claude Code offers the plugin to everyone who trusts the project. To try
a checkout without installing: `claude --plugin-dir agents/claude-code`.
Validate with `claude plugin validate agents/claude-code` and
`claude plugin validate agents` (the marketplace).

## `zell-mcp`

A Model Context Protocol server over stdio (one JSON-RPC message per
line; protocol versions 2025-11-25, 2025-06-18, 2025-03-26, 2024-11-05).
It is built on the compiler library's `Session` API, the same analysis as
`zell-lsp`, and re-reads the project's files from disk before each tool
call, so it always sees the agent's edits. The project root is
`--root <dir>`, else `$ZELL_MCP_ROOT`, else the nearest directory with a
`zell.toml`/`zell.json`/`zell.yaml` above the current directory, else
the current directory. Dependencies from `zell.lock` (`.zell/deps/`,
`vendor/`) and the std sources take part read-only.

| Tool | Arguments | Result |
|---|---|---|
| `diagnostics` | `path?` | `path:line:col: error[E0001]: message`, the source line, notes, quick fixes; the whole project without `path` |
| `definition` | position or `symbol` | kind, full name, location and the first lines of the declaration |
| `references` | position or `symbol`, `include_declaration?` | one `path:line:col: source line` per use |
| `hover` | position or `symbol` | signature, kind, container, type, doc comment, a std example |
| `document_symbols` | `path` | the file's outline with lines and signatures |
| `workspace_symbols` | `query`, `limit?`, `include_libraries?` | fuzzy search over project declarations |
| `rename` | position or `symbol`, `new_name`, `apply?` | unified diffs; a dry run unless `apply: true` |
| `format` | `path?`, `apply?` | `zell fmt` (writes unless `apply: false`), unified diffs |
| `run_tests` | `path?`, `filter?`, `timeout_sec?` | `zell test --json` summarised: counts and failures with output |
| `build` | `path?`, `release?` | `zell build`: success or the compiler errors |
| `run` | `path?`, `args?`, `leak_check?`, `timeout_sec?` | `zell run`: exit code, stdout, stderr (stdin closed) |
| `doc` | `symbol?`, `private?` | what `zell doc <symbol>` prints (namespaces, symbols, the project, `std`) |
| `std_index` | `query?`, `limit?` | public std declarations: kind, full name, signature, summary, source; no query: the namespaces |

A position is `path` + `line` (1-based) + `col` (1-based, characters)
or `name` (the identifier on that line, for when the column is unknown);
instead of a position, `symbol` takes `User`, `User.Hello`,
`app.models.User` or `strings.Split`. Paths in results are relative to
the project root. `run_tests`, `build` and `run` call the `zell` CLI:
`$ZELL_BIN`, else `zell` next to `zell-mcp`, else on `PATH`.

Build and test (from the zell repository, where the crate is a
workspace member):

```sh
cargo build --release -p zell-mcp     # target/release/zell-mcp
cargo test -p zell-mcp                # drives the binary over stdio (tests/server.rs)
```

### Configuration snippets

Codex CLI (`~/.codex/config.toml`, or `codex mcp add zell -- zell-mcp`):

```toml
[mcp_servers.zell]
command = "zell-mcp"
args = []
```

Gemini CLI (`.gemini/settings.json`), Cursor (`.cursor/mcp.json`),
Windsurf (`~/.codeium/windsurf/mcp_config.json`), Cline
(`cline_mcp_settings.json`), Claude Code without the plugin (`.mcp.json`):

```json
{ "mcpServers": { "zell": { "command": "zell-mcp", "args": [] } } }
```

Zed (`settings.json`): `{ "context_servers": { "zell": { "command": "zell-mcp", "args": [] } } }`.
VS Code / Copilot (`.vscode/mcp.json`): `{ "servers": { "zell": { "type": "stdio", "command": "zell-mcp" } } }`.
Gemini CLI can also load `gemini/` as an extension (it bundles the MCP
server and `GEMINI.md`): copy it to `~/.gemini/extensions/zell/` or
`<project>/.gemini/extensions/zell/` (or use `gemini extensions link` /
`install` with the directory, where your Gemini CLI version has them).

## What other languages ship for agents (research, October 2026)

- **Claude Code plugins** declare language servers in `plugin.json`
  (`lspServers`: `command`, `args`, `extensionToLanguage` from extension
  to LSP language id, `initializationOptions`, `settings`, `env`,
  `startupTimeout`, `restartOnCrash`, `maxRestarts`, `diagnostics`) or in
  `.lsp.json`; marketplaces are `.claude-plugin/marketplace.json` at a
  repository root (`claude plugin marketplace add owner/repo`, relative
  `source` paths resolve from the marketplace root; `git-subdir` sources
  fetch one directory of another repository). Anthropic's official
  marketplace has LSP plugins for gopls, rust-analyzer, pyright,
  typescript-language-server, clangd, jdtls, and others; the server binary
  is not bundled, it must be on `PATH`. Plugin LSP servers do not start in
  cloud sessions.
  [Plugin manifest reference](https://code.claude.com/docs/en/plugins-reference),
  [Marketplace reference](https://code.claude.com/docs/en/plugins/marketplace-reference),
  [Code intelligence plugins](https://code.claude.com/docs/en/plugins/code-intelligence).
- **gopls** has a built-in MCP server (v0.20+, experimental): `gopls mcp`
  (detached, stdio, sees saved files) and `gopls serve -mcp.listen=...`
  (attached to the editor's session, sees unsaved edits); tools such as
  `go_workspace`, `go_search`, `go_file_context`, `go_package_api`,
  `go_symbol_references`, `go_diagnostics`; `gopls mcp -instructions`
  prints model instructions (not sent automatically). Zell's design
  follows it: a stdio server over the compiler's own analysis, a
  `diagnostics`-after-edit workflow, and instructions in `initialize`.
  [Gopls: MCP support](https://go.dev/gopls/features/mcp),
  [gopls MCP instructions](https://go.googlesource.com/tools/+show/master/gopls/internal/mcp/instructions.md).
- **rust-analyzer** has no MCP mode of its own; community servers wrap
  its LSP (rust-analyzer-mcp, cursor-rust-tools, cratedocs-mcp), and
  Claude Code uses it through the `rust-analyzer-lsp` plugin.
  [Codex CLI for Rust](https://codex.danielvaughan.com/2026/05/24/codex-cli-rust-development-rust-analyzer-mcp-cargo-systems-programming-agent-patterns/),
  [cursor-rust-tools](https://glama.ai/mcp/servers/@terhechte/cursor-rust-tools/inspect).
- **Codex CLI**: MCP servers in `config.toml` (`[mcp_servers.<name>]`,
  `command`/`args`/`env`/`cwd`, `codex mcp add`), instructions from
  `AGENTS.md` (global, then repository root down to the working
  directory); no built-in LSP as of v0.125 (LSP reaches it only through
  MCP bridges).
  [Codex MCP](https://developers.openai.com/codex/mcp),
  [AGENTS.md in Codex](https://developers.openai.com/codex/guides/agents-md),
  [LSP integration for Codex CLI](https://codex.danielvaughan.com/2026/04/25/codex-cli-lsp-integration-language-server-semantic-code-intelligence/).
- **Gemini CLI**: `mcpServers` in `settings.json`, `GEMINI.md` context
  files (`context.fileName` configurable), extensions with
  `gemini-extension.json` (`mcpServers`, `contextFileName`); IDE
  integration is through a VS Code companion and ACP, not LSP.
  [Gemini CLI configuration](https://www.mintlify.com/google-gemini/gemini-cli/reference/settings),
  [Extensions](https://huggingface.co/Jimmi42/openCLI/blob/main/docs/extension.md),
  [IDE integration](https://geminicli.com/docs/ide-integration.md).
- **Cursor**: `.cursor/mcp.json` (project) and `~/.cursor/mcp.json`;
  rules in `.cursor/rules/*.mdc` with `description`/`globs`/`alwaysApply`
  frontmatter. **Windsurf**: `~/.codeium/windsurf/mcp_config.json`
  (user-level), rules in `.windsurf/rules/`. **Cline**:
  `cline_mcp_settings.json` (now `~/.cline/data/settings/`), rules in
  `.clinerules/`. **Zed**: `context_servers` in settings (not
  `mcpServers`); languages and their servers come from Zed extensions.
  [Cursor MCP](https://www.morphllm.com/cursor-mcp-server),
  [Windsurf MCP](https://fast.io/resources/windsurf-mcp-setup-guide/),
  [Cline configuration](https://docs.cline.bot/cline-cli/configuration.md),
  [Zed MCP](https://zed.dev/docs/ai/mcp).
- **MCP** itself: `initialize` (version negotiation, capabilities,
  `instructions`), `notifications/initialized`, `tools/list`,
  `tools/call` with `content` and `isError`; the stdio transport is
  newline-delimited JSON-RPC; 2025-11-25 is the current revision.
  [MCP 2025-06-18](https://glama.ai/mcp/servers/@chrishayuk/chuk-mcp-server-reference/blob/9ddab97f9b578b38cce2687dadabf9c33da69271/specs/2025-06-18/README.md).

Third-party pages were used where the vendor documentation was not
reachable; check the vendor docs when a path or key changes.

## Gaps

- Claude Code is the only agent here with a native LSP client; the others
  get diagnostics from `zell-mcp` (on request) or from the editor.
- No Zed extension (tree-sitter grammar + `zell-lsp` registration) yet,
  so Zed has MCP but no native Zell LSP.
- `zell-mcp` works on files on disk (no unsaved buffers, unlike gopls'
  attached mode) and has no `resources`/`prompts`; `run_tests`, `build`
  and `run` need the C runtime and `clang`, like the CLI.
- There is no `zell add`: agents edit the manifest and run
  `zell install` (the skill says so).

## Moving to `zell-lang/zell-agents`

The directory is self-contained: nothing here refers to files outside
it, except that `mcp/Cargo.toml` depends on the compiler crate
(`../../compiler`, change it to a git dependency on `zell-lang/zell`) and
the zell repository's `compiler/src/package/agents.rs` embeds the skills
from here for `zell init --agents` (copy them into the compiler, or keep a
synced copy). `templates/AGENTS.md` and `gemini/GEMINI.md` are generated
from the skills: `ZELL_BLESS=1 cargo test -p zell --lib agent_templates`
rewrites them, and the same test fails when they are stale.
