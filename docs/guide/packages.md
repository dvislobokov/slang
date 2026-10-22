# Packages and projects

A Zell **package** is a directory with a manifest and sources. Packages
are identified by their repository path (`github.com/acme/money`), as Go
modules are; there is no central registry to publish to. Versions are
git tags, version selection is Go's minimal version selection, and
`zell.lock` pins exact versions and checksums. The full design is
[docs/PACKAGES.md](../PACKAGES.md) (in Russian).

## A new project

```sh
zell init hello            # zell.toml + src/main.zl
zell init mylib --lib      # a library: src/lib.zl
cd hello && zell run       # build and run
zell build --release       # optimized binary in target/release/
```

`zell init` writes:

```toml
[package]
name = "hello"
version = "0.1.0"
namespace = "hello"
entry = "src/main.zl"
```

`namespace` is the package's root namespace: every file of the package
declares it or a namespace below it (`hello`, `hello.models`,
`hello.http.handlers`). The manifest can also be written as
`zell.json` or `zell.yaml` (`zell manifest convert yaml` switches;
`zell manifest schema` prints the JSON Schema editors use).

## Namespaces and files

Files are grouped by namespace, not by directory; a namespace may span
several files, and any file can import any namespace of the package.
Upper-case names are exported; lower-case names are private to their
namespace.

```zell
// check-only
namespace mylib.money

import std

// Money is an amount in cents.
struct Money {
    Cents int

    // String formats the amount as "12.34".
    func String() string {
        return fmt.Format("{0}.{1}", this.Cents / 100, strings.PadLeft(strconv.Itoa(this.Cents % 100), 2, "0"))
    }
}

// Sum adds amounts.
func Sum(xs []Money) Money {
    var total = 0
    for x in xs {
        total += x.Cents
    }
    return Money{Cents: total}
}
```

Imports shorten names; the full path always works:

| Import | You write |
|---|---|
| none | `std.strings.Split(s, ",")` |
| `import std` | `strings.Split(s, ",")` |
| `import std.strings` | `Split(s, ",")` |
| `import std.strings as str` | `str.Split(s, ",")` |
| `import mylib.money.Money` | `Money{Cents: 5}` |

## Dependencies

Declare dependencies in the manifest, then run `zell install` (builds
also install what is missing):

```toml
[dependencies]
"github.com/acme/money" = "^1.2"                      # git tags v1.2.0 ..< v2
router = { git = "github.com/acme/router", version = "~0.4.1" }
mylib = { path = "../mylib" }                         # a local package

[dev-dependencies]
"github.com/acme/testkit" = "^1"                      # only for zell test
```

Constraints: `^1.2` (>= 1.2, < 2), `~1.2.3` (>= 1.2.3, < 1.3),
`>=1, <2`, `=1.4.0`. A dependency's code is imported by its root
namespace (`import money`); `as` resolves a clash between two roots.

| Command | What it does |
|---|---|
| `zell install [--offline] [--locked]` | fetch modules into the store, link them into `.zell/deps/`, write `zell.lock` |
| `zell update [module ...]` | newest versions within the constraints |
| `zell tree` | the dependency graph |
| `zell why <module>` | every chain of requirements leading to a module |
| `zell vendor` | copy all modules into `vendor/` (builds then use it) |
| `zell store verify` / `prune` / `path` | check, clean up or locate the module store |
| `zell doc <module>` | documentation of a dependency, offline |
| `zell env` | `ZELL_HOME`, `ZELL_STORE`, `ZELL_PROXY`, `ZELL_STD`, ... |

Commit `zell.lock`. Nothing changes until you run `zell update`: a new
release of a dependency is never picked up by surprise. Downloaded files
are checked against the lock and the checksum log (`ZELL_SUMDB`); the
store (`~/.zell/store`) keeps each file once and projects get hard
links, so installs are fast and take no extra space.

Modules come from git (`ZELL_PROXY=direct`, the default) or a module
proxy; `zell proxy serve --dir cache/` runs one for a team or CI.
`ZELL_PRIVATE=github.com/mycorp/*` sends private paths straight to git.

## Workspaces

A repository with several packages has a root manifest listing them;
`zell build`, `zell test` and the editor work across all of them, and
they share one lock file:

```toml
[workspace]
members = ["services/*", "libs/*"]
```

## Features and capabilities

`[features]` turn optional code on and off with `@cfg(feature: "tls")`
on declarations. `[capabilities]` grants the low-level std modules a
package may import: `unsafe = true` for `std.unsafe`, `plugin = true` for
`std.plugin`; without it, importing them is a compile error.

```toml
[features]
default = ["tls"]
tls = []
metrics = []

[capabilities]
unsafe = false
```

## The standard library

The standard library ships as source next to the compiler
(`<prefix>/std/`, or `$ZELL_STD`), so editors can navigate into it and
`zell doc std.net.http` works offline. Functions implemented in the C
runtime have declaration files (`std/strings/strings_extern.zl`) with
their documentation.
