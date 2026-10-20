# Slang example corpus

These programs are both documentation and the end-to-end test suite for the
Slang compiler, runtime and standard library. They follow `DESIGN.md`
exactly; where the design is silent, the choice made is listed in
[Design decisions assumed by the examples](#design-decisions-assumed-by-the-examples).

## Layout

```
examples/
  NAME.sl            single-file program, entry point `main` in the file's namespace
  NAME.out           exact expected stdout of NAME.sl
  multi/             one program made of several files (several namespaces)
    namespaces.sl    namespace app         (contains main)
    models.sl        namespace app.models
    report.sl        namespace app.report
    namespaces.out   expected stdout of the whole directory
  errors_compile/    programs that MUST FAIL to compile (no .out files)
```

## Conventions

- **Runnable programs.** For every `NAME.sl` with a sibling `NAME.out`, the
  test runner does `slang run NAME.sl` and compares stdout byte for byte
  with `NAME.out` (every line, including the last, ends with `\n`). Exit
  code must be 0 and nothing is expected on stderr. All output is
  deterministic: no timing, map-order or address dependence.
- **Multi-file programs.** A directory with a `.out` file is one program:
  the runner compiles all `.sl` files in it together
  (`slang run examples/multi`) and uses `main` from the root namespace
  (`app`). The expected output is `namespaces.out`.
- **Compile-failure programs.** Every file in `errors_compile/` starts with a
  line `// error: <reason>` followed by the `namespace` line. The runner
  must check that `slang build FILE` exits non-zero and reports at least one
  error diagnostic. The text after `// error:` names the expected
  diagnostic; a runner may check that the main diagnostic contains its key
  words, but the exact wording is not normative.
- Every `.sl` file starts with `namespace` (only the leading `// error:`
  comment in `errors_compile/` precedes it).

## Output format (normative for `println`, `fmt.Format`)

The `.out` files depend on these rules; the runtime must follow them.

- `println(a, b, ...)` writes its arguments separated by a single space and
  ends with `\n`, like Go's `fmt.Println`. `println()` writes just `\n`.
- `int` (all integer types): decimal, `-` for negatives.
- `float`: the shortest decimal that round-trips, like Go's `%v`
  (`strconv.FormatFloat(f, 'g', -1, 64)` with Go's `%v` exponent
  thresholds): `0.5` -> `0.5`, `2.0` -> `2`, `1.5e3` -> `1500`,
  `0.1 + 0.2` -> `0.30000000000000004`, `-0.5` -> `-0.5`. Exponent form
  (`1e+21`, `1e-05`) only for exponent < -4 or >= 6, as in Go; the
  examples never hit it.
- `bool`: `true` / `false`. `nil` (pointer or error): `nil`.
- `string`: raw contents, no quotes. `char`: the character itself
  (`'a'` prints `a`, unlike Go's rune which prints a number).
- Slices and arrays: Go style, `[1 2 3]`, nested `[[1 2] [3 4]]`.
- Plain enum values: the variant name (`Color.Green` prints `Green`).
- Any value whose static type is `error` (or a struct used through the
  `error` interface): the result of its `Message()`.
- `fmt.Format(tmpl, args...)`: each `{N}` is replaced by argument `N`
  formatted exactly as `println` would format it; other text is copied
  verbatim (`$` has no special meaning).
- Maps and structs are never printed directly by the examples.

## Examples

| File | Features exercised |
|------|--------------------|
| `hello.sl` | `namespace`, `import std.io`, bare `println` |
| `arith.sl` | int/float literals, `+ - * / %`, truncating division and sign of `%`, Go precedence (incl. `<<`, `&`, `\|`, `^`), compound assignment, top-level and local `const`, `var` with inferred type, explicit type and zero value, float printing, `math.Sqrt/Pow/Floor/Abs`, `float()`/`int()` conversions, comparisons, `&&`/`\|\|`/`!` precedence |
| `control.sl` | `if`/`else if`/`else` (all branches return), `for i in a..b` with literal and variable bounds, `for i, x in slice`, `for x in [literal]`, while-style `for cond`, infinite `for` with `break`, `continue`, nested loops |
| `functions.sl` | grouped params `(a, b int)`, multiple results and `var (a, b) = f()`, `_` in destructuring, recursion (factorial, fib), block lambda `func(x int) int {}`, arrow lambda `(x int) => x * x`, lambdas as arguments, returning closures (`Compose`, counter with captured state), function types `func(int) int` / `func(string)`, named functions as values, slice of functions |
| `structs.sl` | fields and methods with `this`, `static func` constructors, struct literals incl. empty `Point{}`, methods that mutate the receiver, value copy on assignment and by-value parameters, `&`, `*p`, `(*p).Y = ...`, auto-deref `p.X`, methods through pointers, `new`, pointer aliasing, `nil` and `== nil`, linked list of `*Node`, embedding with promoted fields/methods and explicit `d.Animal.Name` |
| `interfaces.sl` | interface declaration, structural implementation by two (three) structs, function taking an interface, `[]Shape` of mixed values, type switch `case Circle c:` with `default`, a struct satisfying two interfaces, interface holding a `*Circle` |
| `enums.sl` | plain enum, `Color.Red`, `==` on enums, `int(Color.Blue)`, printing an enum, enum with fields (`Circle(radius float)`), plain variant inside a data enum, constructing variants, `case Result.Ok(v):` destructuring with one and two fields, `default`, nested `if` inside a case |
| `slices_maps.sl` | slice literal, `append`, `len`, indexing and index assignment, sub-slice `s[1..4]` sharing the buffer, empty `[]` vs zero-value slice, nested slices, fixed array `[3]int`, map literal, empty map literal, set/get/overwrite, missing key -> zero value, `delete`, deterministic printing via a key list, order-independent `for _, v in m` and `for k in m`, `m[k] += 1`, reference semantics of maps, map of slices |
| `strings.sl` | `+` concatenation, `len` (bytes), `==`/`<`/`>`, `strings.Split/Join/Trim/Contains/ToUpper/Replace/ToInt`, `try ... else` with `ToInt`, `for ch in s` over code points (non-ASCII `héllo`), `switch` on chars, `string(ch)`, `+=` on strings, `fmt.Format` with reordered/repeated placeholders, escapes `\t \" \\` |
| `errors.sl` | `T!` and `void!` functions, `error("msg")`, `fmt.Format` inside errors, `?`, `?("context")`, `return f()?(...)`, `try ... catch err { return }`, statement-level `try ... catch`, `try ... else value`, `try ... else continue`, `continue` from a `catch` block, custom error structs with `Message()`, `switch err { case NotFound e: ... }`, sentinel `const ErrNotFound = error(...)`, `errors.Is` through wrapping, `errors.As(err, &target)`, `errors.Wrap/Unwrap/New`, `Unwrap` of an unwrapped error -> `nil`, `_ = f()`, top-level `var` |
| `switch.sl` | value cases, value lists, half-open ranges incl. negative bounds, guards (`case n > 100:`), first-match ordering, string cases, no-subject `switch { case cond: }`, multi-statement bodies, no fallthrough, switch with no match and no default, FizzBuzz |
| `defer.sl` | LIFO order, defer in a loop, arguments evaluated at `defer`, deferred closure `defer func() { ... }()` seeing later values, deferring a call of a lambda variable, defers on early `return error(...)`, defer in `main` |
| `async.sl` | `async func`, `async func main`, `await`, `task.Delay`, concurrent tasks with `task.All` and tuple destructuring, `await f()?`, `try await ... catch`, `try await ... else`, `task.All` over fallible tasks with `try`, sequential awaits in a loop |
| `multi/` | three files, three namespaces: `import app.models` short names, full path `app.models.User.New(...)` even though imported, `import std.fmt as f` alias, calling non-imported `app.report.Summary` by full path, a file with no imports using `std.io.println` and `app.models.User` as a parameter type, private field `id` set only inside its namespace, exported `const` |
| `bank.sl` | larger program: `Account` with private fields, `void!` methods, `static func` returning `*Account`, enum `Tx` with fields and plain enum `EntryKind`, `Bank` with `map[string]*Account` and `[]string`, typed error `InsufficientFunds`, sentinel + `errors.Wrap`, `?` and `?("context")` on method calls through map elements, `errors.As` / `errors.Is` in a `catch`, switch over transactions and history, integer-cents money formatting, final report |
| `errors_compile/unused_var.sl` | declared and never used local variable |
| `errors_compile/unhandled_result.sl` | `int!` value used without `?`/`try` |
| `errors_compile/unhandled_void_result.sl` | `void!` call used as a statement without `?`/`try`/`_ =` |
| `errors_compile/question_outside_fallible.sl` | `?` inside a function that does not return `T!` |
| `errors_compile/await_outside_async.sl` | `await` in a non-`async` function |
| `errors_compile/unknown_ident.sl` | reference to an undeclared name |
| `errors_compile/type_mismatch.sl` | assigning a `string` to an `int` variable |
| `errors_compile/missing_return.sl` | `if`/`else if` without final `else` in a function returning `int` |
| `errors_compile/catch_falls_through.sl` | `var n = try f() catch err { ... }` whose block neither returns nor jumps |

## Standard library functions used by the examples

The examples use only these functions. "As println" means formatted
per [Output format](#output-format-normative-for-println-fmtformat).

**Builtins** (always in scope, no import)

| Function | Signature / behaviour |
|----------|-----------------------|
| `error` | `error(msg string) error` — new error whose `Message()` is `msg` |
| `len` | `len(x) int` for `string` (bytes), `[]T`, `[N]T`, `map[K]V` |
| `append` | `append(s []T, x T) []T` (Go semantics; nil/zero slice is fine) |
| `delete` | `delete(m map[K]V, k K)`; no-op if absent |
| conversions | `float(int) float`, `int(float) int` (truncates toward zero), `int(enum) int` (variant ordinal from 0), `string(char) string` |

**std.io** — `println(args ...any)` (also reachable as `std.io.println`).

**std.fmt** — `Format(template string, args ...any) string`, placeholders `{0}`, `{1}`, ...

**std.strings**

| Function | Signature / behaviour |
|----------|-----------------------|
| `Split` | `Split(s, sep string) []string` (as Go `strings.Split`) |
| `Join` | `Join(parts []string, sep string) string` |
| `Trim` | `Trim(s string) string` — removes leading and trailing whitespace (like Go `TrimSpace`; no cutset argument) |
| `Contains` | `Contains(s, substr string) bool` |
| `ToUpper` | `ToUpper(s string) string` |
| `Replace` | `Replace(s, old, new string) string` — replaces all occurrences |
| `ToInt` | `ToInt(s string) int!` — base-10, optional leading `-`; fails on anything else (the error text is not relied on) |

**std.math** — `Sqrt(x float) float`, `Pow(x, y float) float`,
`Floor(x float) float`, `Abs(x float) float`.

**std.errors**

| Function | Signature / behaviour |
|----------|-----------------------|
| `New` | `New(msg string) error` — same as builtin `error(msg)` |
| `Wrap` | `Wrap(err error, msg string) error` — `Message()` is `msg + ": " + err.Message()`; keeps `err` as the cause |
| `Unwrap` | `Unwrap(err error) error` — the cause, or `nil` if `err` is not a wrapper |
| `Is` | `Is(err, target error) bool` — true if `target` equals `err` or any cause in its chain |
| `As` | `As(err error, target *T) bool` — `T` is a struct type implementing `error`; if some error in the chain has dynamic type `T`, copies it into `*target` and returns true |

**std.task**

| Function | Signature / behaviour |
|----------|-----------------------|
| `Delay` | `Delay(ms int) Task<void>` — completes after `ms` milliseconds (0 allowed) |
| `All` | `All(t1 Task<T1>, ..., tn Task<Tn>) Task<(T1, ..., Tn)>` — waits for all; if any `Ti` is fallible (`Ui!`) the result is `Task<(U1, ..., Un)!>` and fails with the error of the first failing task in argument order |

## Design decisions assumed by the examples

These are choices made where `DESIGN.md` is silent or ambiguous; each is
consistent with Go unless stated. Please fold them into the design.

1. **Import rule is applied literally.** `import std.io` gives bare
   `println`; `import std` gives `strings.Split`, `fmt.Format`,
   `math.Sqrt`, `errors.Is`, `task.All`. So most examples use
   `import std.io` + `import std`. (The example program at the end of
   DESIGN.md writes `import std.strings` and then `strings.Split`, which
   contradicts the import table; the examples follow the table.)
2. **Entry point.** For a single file, `main` is taken from the file's
   namespace. `main` may be `async func main()`; the runtime awaits it.
3. **Builtins** `error`, `len`, `append`, `delete`, `panic` and the
   conversions `float(x)`, `int(x)`, `string(c)` are always in scope.
   There is no implicit int/float conversion between typed values; untyped
   numeric constants convert implicitly as in Go (`Point.New(3, 4)` with
   `float` params, `Point{X: 1, Y: -2}`).
4. **Integer arithmetic** is Go's: `/` truncates toward zero, `%` has the
   sign of the dividend; precedence table as in `ast.rs` (Go's).
5. **Ranges `a..b` are half-open everywhere**: in `for`, in `case` patterns
   (`case 3..10:` matches 3 to 9) and in sub-slicing.
6. **Sub-slicing is `s[a..b]`** (the AST's `Range` expression used as an
   index), meaning Go's `s[a:b]`, sharing the underlying buffer. Open-ended
   forms are not used. Note that `grammar/slang.bnf` currently has no range
   expression at all (not in `for ... in` either); the AST's `Range` is
   what the examples rely on.
7. **Strings**: `len(s)` is the byte length; `for ch in s` yields `char`
   code points; `for _ in s` is allowed. String indexing `s[i]` is not used.
   Comparison is byte-wise lexicographic.
8. **Maps**: reading a missing key yields the zero value without inserting;
   `m[k] += 1` works on missing keys; iteration order is unspecified
   (examples only aggregate when iterating); `for k in m` yields keys,
   `for k, v in m` yields key and value. No comma-ok lookup is used.
9. **`for` variables**: `for i, x in slice` gives index and element; `_` may
   be used for either; each iteration gets a fresh variable (Go 1.22).
10. **Struct literals are not allowed directly in `if`/`for`/`switch`
    headers** (the Go rule, also noted in `grammar/slang.bnf`), so
    `for x in list {` and `switch n {` parse as expected.
11. **Methods and interfaces**: methods have pointer receivers; calling a
    method on an addressable value (`p.Move()`) takes its address
    implicitly; both a `T` value and a `*T` satisfy an interface whose
    methods `T` declares. Storing a value in an interface copies it;
    storing a pointer shares it. A type switch `case Circle c:` matches a
    stored `Circle` value and binds a copy.
12. **Embedding**: an embedded struct is initialised in a literal by its
    type name (`Dog{Animal: Animal{Name: "Rex"}}`) and is accessible as the
    field `d.Animal`.
13. **Enums**: plain variants compare with `==`, print as their name and
    convert to their 0-based ordinal with `int(...)`. A data enum may also
    have plain variants (`Empty`), written without parentheses. A switch
    over an enum need not be exhaustive (the examples add a trailing
    `return` after such switches).
14. **Terminating statements** (for "missing return"): as in Go, a
    `return`, an `if` with `else` whose branches all terminate, and a
    `switch` with `default` whose cases all terminate.
15. **Switch**: cases are tried top to bottom, first match wins. A case
    expression of type `bool` in a switch whose subject is not `bool` is a
    guard. A switch with no matching case and no `default` does nothing.
    A case body is every statement up to the next `case`/`default`.
16. **Errors**:
    - `void!` functions may fall off the end (success), like `void`.
    - A call returning `T!` or `void!` must be handled with `?`, `try` or
      `_ =`; a bare expression statement is a compile error too.
    - `try f() catch err { ... }` as a statement discards the success value.
    - When the value of `try ... catch` is used (`var x = try ...`), the
      catch block must not fall through: it must end in `return`,
      `panic`, `break` or `continue` (the design lists only
      return/panic; `continue` is used by the examples, mirroring
      `try ... else continue`).
    - `f()?("ctx")` and `errors.Wrap(err, "ctx")` produce the message
      `"ctx: " + inner message` and keep the inner error as the cause.
    - `return X` in a `T!` function returns an error when `X`'s type
      implements `error`, otherwise a value.
    - `errors.Is` is used only with the very same `const` sentinel, so
      either identity or by-value comparison works.
    - `errors.Unwrap` of a non-wrapper returns `nil`, printed as `nil`.
17. **Async**: calling an `async func` starts the task immediately; `await
    f()?` means `(await f())?` (the parser yields `Await(Propagate(...))`;
    semantics apply `?` to the awaited result); `try await f() catch` /
    `else` work like on any `T!`. Examples print only from the main task
    or from a task that is awaited immediately, so output is ordered.
18. **Defer**: Go semantics — the deferred call's function value and
    arguments are evaluated at the `defer` statement, calls run LIFO when
    the function returns (including early error returns), defers in loops
    accumulate, closures see the variables' final values, and `main`'s
    defers run before the program exits.
19. **Closures** capture variables by reference. Arrow lambdas infer their
    return type from the body expression.
20. **Multi-line slice literals** use a trailing comma, so Go-style
    semicolon insertion after `)` at line end does not break them.
21. **Unused names**: unused local variables are errors; unused function
    parameters, functions and results of non-fallible calls (`counter()`)
    are not. Every import in the examples is used, so either policy on
    unused imports works.
