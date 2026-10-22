---
name: zell-packages
description: Manage Zell projects and dependencies - zell.toml/zell.json/zell.yaml manifests, module paths like github.com/org/pkg, version constraints, zell.lock, workspaces, features and capabilities, and the zell init/install/update/tree/why/vendor commands. Use when adding, upgrading, explaining or debugging Zell dependencies or project layout.
---

# Zell packages and projects

A **package** is a directory with one manifest (`zell.toml`, or
`zell.json` / `zell.yaml`: same schema, `zell manifest schema` prints
it) and sources in `src/`. Packages are identified by their repository
path (`github.com/acme/money`), like Go modules: there is no central
registry; versions are git tags (`v1.2.0`), selection is Go's minimal
version selection, and `zell.lock` pins exact versions and checksums.

## Manifest

```toml
[package]
name = "shop"               # [a-z0-9-], starts with a letter
version = "0.3.1"
namespace = "shop"          # root namespace: every file declares shop or shop.x.y
entry = "src/main.zl"       # binary; omit for a library (src/lib.zl)
license = "MIT"

[dependencies]
"github.com/acme/money" = "^1.2"                         # git tags v1.2.0 ..< v2
router = { git = "github.com/acme/router", version = "~0.4.1" }   # alias name
mylib = { path = "../mylib" }                             # local package
extra = { git = "https://github.com/acme/extra", tag = "v0.4.0" }

[dev-dependencies]
"github.com/acme/testkit" = "^1"                          # only for zell test

[features]
default = ["tls"]
tls = []                     # code under @cfg(feature: "tls")

[capabilities]
unsafe = false               # true allows `import unsafe`; plugin = true allows `import plugin`

[workspace]
members = ["services/*", "libs/*"]   # root manifest of a multi-package repo; one shared zell.lock
```

Constraints: `^1.2` (>= 1.2, < 2), `~1.2.3` (>= 1.2.3, < 1.3),
`>=1, <2`, `=1.4.0`. One major version of a package per graph.

## Namespaces vs module paths

- The module path (`github.com/acme/money`) only says where to fetch;
  code is imported by the dependency's **root namespace** from its
  manifest: `import money`, `money.Sum(xs)`. Use `import money as m`
  when two roots clash.
- Standard library namespaces (`io`, `strings`, `net`, `task`, ...) are
  top-level and reserved: a package cannot use one as its root, and
  there is no `std.` prefix.
- By convention a file `src/a/b/x.zl` of package root `shop` declares
  `namespace shop.a.b` (the compiler does not require it; the LSP's
  file-move refactoring relies on it).
- A package sees only the dependencies its manifest declares (no
  phantom dependencies).

## Commands

| Command | What it does |
|---|---|
| `zell init [dir] [--lib] [--format toml\|json\|yaml]` | new package: manifest + `src/main.zl` (or `src/lib.zl`) |
| `zell install [--offline] [--locked]` | fetch into the store (`~/.zell/store`), hard-link into `.zell/deps/`, write `zell.lock` |
| `zell update [module\|name ...]` | newest versions within the constraints (all, or the named ones) |
| `zell tree` | the dependency graph |
| `zell why <module\|name>` | every chain of requirements that pulls a package in |
| `zell vendor` | copy all modules into `vendor/` (builds then use it) |
| `zell store verify \| prune \| path` | check, clean up, locate the store |
| `zell env [NAME]` | `ZELL_HOME`, `ZELL_STORE`, `ZELL_PROXY`, `ZELL_STD`, ... |
| `zell manifest convert <fmt>` | rewrite the manifest in another format |
| `zell doc <namespace>` | a dependency's API, offline |

There is no `zell add`/`zell get` yet: **edit the manifest, then run
`zell install`** (builds also install what is missing). `--locked` fails
when the lock would change (CI); `--offline` uses only the lock and the
store.

## Rules for agents

- Commit `zell.lock`; never commit `.zell/` or `target/`.
- Never edit files under `.zell/deps/` or `vendor/`: they are read-only
  copies (the LSP refuses to rename into them).
- Nothing upgrades until `zell update`: to explain a version, run
  `zell why <module>` and `zell tree` instead of guessing.
- `ZELL_PROXY=direct` (default) fetches from git; `ZELL_PRIVATE=github.com/corp/*`
  sends private paths straight to git; `zell proxy serve --dir cache/`
  runs a team proxy.
- In a workspace, `zell build/test --workspace` or `-p <member>` select
  members; all share the root `zell.lock`.
