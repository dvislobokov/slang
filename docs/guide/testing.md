# Testing

`slang test` finds the tests of a package (or a directory, or one file),
builds them into one binary and runs every test in its own process, so a
failed assertion, a `Fatal` or even a panic ends only that test.

```sh
slang test                     # the package of the current directory
slang test path/to/dir -v      # every test, with names and logs
slang test --filter '^TestParse|Sum$'
slang test --json              # one JSON event per line (CI, editors)
slang test --bench             # also run the benchmarks
slang test --parallel 8        # parallel tests at a time (default 4)
slang test std                 # the standard library's own tests and examples
```

## Tests

A test is a function marked `@test` that takes a `*testing.T` (or no
parameters at all). Assertions from `std.testing` report a failure and
keep going; their `...Fatal` variants stop the test.

```slang
// slang test
namespace guide.testing.basics

import std

func Add(a int, b int) int {
    return a + b
}

func Parse(s string) int! {
    return strings.ToInt(s)?("parse")
}

@test func TestAdd(t *testing.T) {
    testing.Equal(t, Add(2, 3), 5)
    testing.NotEqual(t, Add(2, 2), 5)
}

@test func TestParse(t *testing.T) {
    var n = try Parse("42") catch err {
        t.Fatal("unexpected error: " + err.Message())
        return
    }
    testing.Equal(t, n, 42)
    try Parse("x") catch err {
        testing.Contains(t, err.Message(), "parse")
    }
}

@test func plainTestsNeedNoHandle() {
    if Add(1, 1) != 2 {
        panic("arithmetic is broken")
    }
}
```

`t.Log(msg)` prints under the test (shown for failures and with `-v`),
`t.Error(msg)` marks it failed and continues, `t.Fatal(msg)` stops it,
`t.Skip(msg)` skips the rest. Other assertions: `True`, `False`, `Nil`,
`NotNil`, `EqualSlices`, `Error`, `NoError`, `ErrorIs` and
`Panics(t, fn)`, which checks that `fn` panics (it re-runs the test in a
child process up to that call).

## Table tests and subtests

Tables are slices of structs; `t.Run(name, fn)` runs a named subtest
(`TestAbs/negative` in the output). A test whose body calls
`t.Parallel()` runs concurrently with the other parallel tests.

```slang
// slang test
namespace guide.testing.table

import std
import std.io

struct Case {
    Name string
    In   int
    Want int
}

func abs(x int) int {
    if x < 0 {
        return -x
    }
    return x
}

@test func TestAbs(t *testing.T) {
    t.Parallel()
    var cases = [Case{Name: "negative", In: -3, Want: 3}, Case{Name: "zero", In: 0, Want: 0}, Case{Name: "positive", In: 5, Want: 5}]
    for c in cases {
        t.Run(c.Name, func(t *testing.T) {
            testing.Equal(t, abs(c.In), c.Want)
        })
    }
}

@test func TestPanics(t *testing.T) {
    var xs = [1, 2, 3]
    testing.Panics(t, func() {
        println(xs[abs(-7)])
    })
}
```

## Benchmarks

A benchmark takes a `*testing.B` and runs the measured code `b.N` times;
`slang test --bench` reports time and allocations per operation.
`b.ResetTimer()`, `StopTimer()` and `StartTimer()` exclude setup.

```slang
// slang test
namespace guide.testing.bench

import std

@bench func BenchJoin(b *testing.B) {
    var parts = ["a", "b", "c", "d"]
    b.ResetTimer()
    for _ in 0..b.N {
        _ = strings.Join(parts, ",")
    }
}

@test func TestJoin(t *testing.T) {
    testing.Equal(t, strings.Join(["a", "b"], ","), "a,b")
}
```

## Examples

A function `ExampleName()` in a `*_test.sl` file is an *example*: it is
compiled with the tests, `slang doc` shows it under the symbol it names,
and if it ends with an `// Output:` comment `slang test` runs it and
compares its standard output with the comment (surrounding white space
ignored; `// Unordered output:` compares the lines in any order).

| Function | Documents |
|---|---|
| `Example()` | the namespace |
| `ExampleSplit()` | function or type `Split` |
| `ExampleBuilder_Write()` | method `Builder.Write` |
| `ExampleSplit_empty()` | `Split` again; the lower-case suffix names the variant |

An example may also be `async` or return `void!` (an error fails it).

```slang
// slang test
namespace guide.testing.examples

import std
import std.io

// Greet returns a greeting for name.
func Greet(name string) string {
    return "Hello, " + name + "!"
}

func ExampleGreet() {
    println(Greet("Ann"))
    println(Greet("Bob"))
    // Output:
    // Hello, Ann!
    // Hello, Bob!
}

func ExampleGreet_unordered() {
    for name in ["x", "y"] {
        println(Greet(name))
    }
    // Unordered output:
    // Hello, y!
    // Hello, x!
}
```

The standard library documents itself this way: `std/**/*_test.sl` hold
examples for the most used functions, and `slang test std` runs them.

## Output and exit status

Without `-v`, passing tests are quiet; a failure prints the test's log and
`--- FAIL: TestName (0.00s)`, and the run ends with a summary such as
`ok   12 passed, 0 failed, 1 skipped (0.31s)`. The exit status is 1 when
anything failed, so `slang test` works in CI as is; `--json` gives
machine-readable events (`{"Action":"pass","Test":"TestAdd","Elapsed":0.004}`).
