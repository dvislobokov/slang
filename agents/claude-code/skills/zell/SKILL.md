---
name: zell
description: Write, fix, test and review Zell code (.zl files, zell.toml projects). Use whenever a task touches Zell sources or the zell CLI - syntax, namespaces and imports, T! errors with ? and try, async/await, memory (ARC, Drop, weak), @test tests, zell build/run/test/fmt/doc, and the println output format.
---

# Zell for coding agents

Zell is a compiled language: Go's small core and semantics, C#-style
`namespace`/`import` and `var`, methods inside structs, `T!` errors with
`?`/`try`, `async`/`await` on a task scheduler, generics, and no garbage
collector (reference counting + `Drop` + `weak`). Where these notes are
silent, **assume Go semantics** (integer division, `switch` without
fallthrough, `defer`, maps, slices, upper-case export).

Ground truth beats memory: check APIs with `zell doc strings.Split`
(or the `doc` / `std_index` MCP tools) and type-check with `zell check`
(or the `diagnostics` tool) after every edit.

## Files, namespaces, imports

- Every file starts with `namespace a.b.c`. Directories do not matter;
  a namespace may span files; any file can import any namespace.
- Standard library namespaces are **top-level**: `io`, `strings`, `fmt`,
  `slices`, `maps`, `task`, `errors`, `net.http`, `encoding.json`, ...
  There is **no `std.` prefix** (`import std.io` is an error: use
  `import io`).
- A full path always works without an import: `strings.Split(s, ",")`,
  `app.models.User{}`. `import strings` lets you write `Split(...)`;
  `import strings as str`, `import app.models.User` (one name) also work.
- `println`/`print` live in `io`: write `import io` (or `io.println`).
- Unused imports and unused local variables are **compile errors**
  (rename to `_` or remove). Upper-case names are exported; lower-case
  are private to their namespace.

## Syntax in one program

```zell
namespace app

import io

const MaxUsers = 100

// User is a registered user. (doc comments: `//` right above)
struct User {
    Name  string
    Email string
    age   int // private

    func Domain() string {              // method: receiver is `this` (a pointer)
        var at = strings.Index(this.Email, "@")
        return this.Email[at + 1..]     // s[a..b] substring / sub-slice
    }

    static func New(name string, email string) *User {
        return new User{Name: name, Email: email}   // new: heap, *User
    }
}

interface Named {                       // structural, like Go
    func Domain() string
}

enum Shape {                            // enums with data, exhaustive switch
    Circle(r float)
    Rect(w float, h float)

    func Area() float {
        switch this {
            case Shape.Circle(r): return 3.0 * r * r
            case Shape.Rect(w, h): return w * h
        }
    }
}

func Max<T: Ordered>(a T, b T) T {      // generics (monomorphized)
    if a > b {
        return a
    }
    return b
}

func main() {
    var n = 42                          // only `var`; no `:=`
    var xs []int = []                   // type needed when the value does not say it
    xs = append(xs, n, 7)
    var ages = map[string]int{"ann": 31}
    ages["bob"] += 1
    for i, x in xs {                    // also: for x in xs, for i in 0..n, for cond {}
        println(i, x)
    }
    var (q, r) = (n / 5, n % 5)         // tuples
    var sq = slices.Map(xs, (x int) => x * x)   // arrow lambda; block: func(x int) int { ... }
    var named Named = User.New("ann", "a@b.org")
    println(q, r, sq, Max(3, 7), named.Domain(), Shape.Rect(2, 3).Area())
    println(fmt.Format("{0} has {1} users", "app", len(ages)))
    defer println("bye")                // Go defer
}
```

`switch` takes values, lists, ranges, guards (`case n > 100:`), no
subject (`switch { case cond: }`) and type switches on interfaces
(`case Circle c:`). `nil` exists only for pointers, interfaces, slices,
maps and functions. Statements end at newlines (Go-style automatic `;`);
a line ending in `.` continues on the next line.

## Errors: `T!`, `?`, `try`

```zell
namespace app

import io

func parsePort(s string) int! {                    // int or an error; void! = nothing or an error
    var n = strings.ToInt(s)?("port " + strconv.Quote(s))   // ? propagates, ?("ctx") wraps
    if n < 1 || n > 65535 {
        return error(fmt.Format("port {0} out of range", n))
    }
    return n
}

func main() {
    var p = try parsePort("80") catch err {        // catch must leave: return/continue/break/panic
        println("error:", err)
        return
    }
    var q = try parsePort("x") else 8080           // default on error
    try {                                          // block try: ? jumps to the catch
        var a = strings.ToInt("1")?
        var b = strings.ToInt("oops")?
        println(a + b)
    } catch err {
        println("stopped:", err)
    }
    _ = p + q
}
```

- A `T!` result cannot be ignored: handle it, `?` it, or `_ = f()`.
- `?` needs an enclosing `T!` function or a block `try`.
- `main` may return `void!`: an error prints `error: <msg>` + trace, exit 1.
- `errors.Is(err, Sentinel)`, `errors.As(err, &target)`, `errors.Wrap`;
  custom errors are structs with `func Message() string`;
  sentinels: `const ErrNotFound = error("not found")`.
- `panic(msg)` is for bugs only.

## Async

```zell
namespace app

import io

async func fetch(name string, ms int) string {   // returns Task<string> when called
    await task.Delay(ms)
    return name + " done"
}

async func main() {
    var a = fetch("a", 20)                       // starts now
    var b = fetch("b", 10)
    println(await b, await a)
    var (x, y) = await task.All(fetch("x", 5), fetch("y", 1))
    println(x, y)
}
```

Channels `task.Channel<T>(n)` (`await ch.Send(v)`, `for v in ch`,
`ch.Close()`), `task.Mutex`, `task.WaitGroup`, `task.Any`,
`task.WithTimeout(ms, async func() T! {...})`, cancellation errors
`task.ErrCancelled`; the task context (cancellation, `task.Key`/`With`/
`Get`) is implicit, never a parameter. `try await f() catch err {}` works.

## Memory (no GC)

- Structs, arrays, numbers, tuples are **values** (copied on
  assignment). `new T{}`, slices, maps, strings, closures, errors,
  interface boxes are **reference counted** and freed when the last
  reference goes. Slices share buffers like Go.
- `func Drop()` in a struct is a destructor: runs exactly once, at block
  end for values, at the last reference for heap objects. Use it for
  files, sockets, locks.
- Break reference cycles with `weak *T` back pointers (parent links,
  observers); it reads as `nil` once the target is gone. The compiler
  warns `W0001` for types that can form cycles; a bounded cycle
  collector exists, but prefer `weak`.
- Interior pointers (`&x.field`, `&xs[i]`) may be used and passed down
  but never stored in fields/slices/closures or returned ("cannot keep
  an interior pointer"): use `new` instead.
- `zell run --leak-check` reports objects alive at exit (exit 66).

## Tests

Tests live in `*_test.zl` files of the same namespace:

```zell
namespace calc

import io

func Add(a int, b int) int {
    return a + b
}

@test func TestAdd(t *testing.T) {
    testing.Equal(t, Add(2, 3), 5)                 // also NotEqual, True, False, Nil, Error, NoError, ErrorIs, Panics
    t.Run("negative", func(t *testing.T) {         // subtests; t.Parallel(), t.Log/Error/Fatal/Skip
        testing.Equal(t, Add(-1, -1), -2)
    })
}

@bench func BenchAdd(b *testing.B) {
    for _ in 0..b.N {
        _ = Add(1, 2)
    }
}

func ExampleAdd() {                                // runs and compares with // Output:
    println(Add(1, 2))
    // Output:
    // 3
}
```

`zell test [path] [--filter re] [-v] [--json] [--bench]`. Exit status 1
on failure; `--json` gives one event per line
(`{"Action":"fail","Test":"TestAdd",...}`).

## CLI

| Command | Use |
|---|---|
| `zell check [path]` | type-check only (fast; run after edits) |
| `zell run [path] [-- args]` | build and run (file, directory, or project) |
| `zell build [path] [--release] [-o out]` | compile |
| `zell test [path] [--filter re] [--json]` | tests, benchmarks (`--bench`), examples |
| `zell fmt [paths] [--check] [--diff]` | the only style; no options. Format before committing |
| `zell doc <ns or symbol>` | offline API docs: `zell doc strings`, `zell doc net.http.Get`, `zell doc std` |
| `zell init [dir] [--lib]` | new project (`zell.toml` + `src/main.zl`) |
| `zell install`, `zell tree`, `zell why <m>` | dependencies (see the zell-packages skill) |

`path` is a file (one program), a directory without a manifest (all its
`.zl` files are one program) or a project (nearest `zell.toml`).
Compiler errors look like `error: msg` + `--> file:line:col`, often with
`help:` lines naming the fix (e.g. the missing `import`).

## Output format (println, fmt.Format)

- `println(a, b)` separates with one space, ends with `\n`.
- floats print like Go `%v`: shortest round-trip, `2.0` -> `2`,
  `1.5e3` -> `1500`, `0.1 + 0.2` -> `0.30000000000000004`; exponent form
  when the exponent is < -4 or >= 6 (`1e+06`, `1e-05`).
- slices `[1 2 3]` (nested `[[1 2] [3 4]]`); `char` prints the character
  (not a number); strings print raw; plain enums print the variant name;
  `nil` prints `nil`; an `error` prints its `Message()`.
- `fmt.Format("{0} {1}", a, b)` formats each `{N}` like `println`.

## Workflow

1. Read the namespace/imports of the file you edit; follow the
   neighbours' style.
2. After each edit: `zell check` (or the `diagnostics` MCP tool / the
   LSP diagnostics Claude Code shows after edits). Fix every error and
   warning; the `help:`/fix lines usually say how.
3. `zell test` for the package; add `@test` functions for new behaviour.
4. `zell fmt` the files you touched.
5. Unsure about a std API? `zell doc <name>` or `std_index` before
   writing code; do not invent functions.
