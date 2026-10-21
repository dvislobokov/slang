# Debugging

What to reach for, from the quickest to the most thorough: read the
error trace, look at the panic backtrace, log, check for leaks, step
through the program in the debugger, profile.

## Error traces

An error remembers where it was created and every `?` it passed through.
When `main` returns an error, the program prints the message and the
trace to stderr and exits with status 1:

```text
error: config: open app.toml: not found
  created at   app/fs.sl:42      open
  returned via app/config.sl:17  readConfig  ?("config")
  returned via app/main.sl:9     main
```

In code, `errors.Trace(err)` returns the same text and
`errors.Frames(err)` the records (see [errors.md](errors.md)):

```slang
namespace guide.debugging.trace

import std
import std.io

func step(n int) int! {
    if n == 0 {
        return error("reached zero")
    }
    return step(n - 1)?
}

func main() {
    try step(2) catch err {
        println(err, "after", len(errors.Frames(err)), "frames")
        for f in errors.Frames(err) {
            println(" ", f.Func, f.Line > 0)
        }
    }
}
// Output:
// reached zero after 4 frames
//   step true
//   step true
//   step true
//   main true
```

## Panics

A panic (`panic(msg)`, an index out of range, a `nil` dereference,
division by zero) prints `panic: <message>` and a native backtrace, and
exits with status 2. Deferred calls and destructors do not run. Set
`SLANG_NO_BACKTRACE=1` to print only the message (the test suites do).

## Logging

`std.log` is structured logging in the style of Serilog: message
templates are checked by the compiler, properties are kept as values
(for JSON sinks), and a disabled level costs nothing because the
arguments are not evaluated. `log.Info("...")` uses the default logger
(stderr); configure your own for anything else.

```slang
namespace guide.debugging.log

import std

struct Order {
    Id    int
    Total float
}

func main() {
    var logger = log.New().MinimumLevel(log.Level.Debug).WriteTo.Console(log.ConsoleOptions{
        Template: "[{Level:u3}] {Message:lj}{NewLine}",
        Stdout: true,
    }).CreateLogger()
    var o = Order{Id: 7, Total: 19.5}
    logger.Info("order {@Order} paid: {Amount:0.00}", o, o.Total)
    logger.Debug("cache size {Size}", 128)
    logger.Verbose("not shown: below the minimum level")
}
// Output:
// [INF] order Order { Id: 7, Total: 19.5 } paid: 19.50
// [DBG] cache size 128
```

Sinks write text or JSON (CLEF) to the console or rolling files;
`log.Context.Push("RequestId", id)` adds a property to everything logged
by the current task and the tasks it starts.

## Leaks

Slang has no garbage collector, so a reference cycle without `weak` is a
leak (see [memory.md](memory.md)). Run with `--leak-check` to find them:

```sh
slang run --leak-check app.sl
```

At exit the program prints `leak: N objects` and, in a debug build, a
line per type still alive and the field that closes the cycle, and
exits with status 66. This program leaks two nodes:

```slang
// check-only
namespace guide.debugging.leak

import std.io

struct Node {
    Name string
    Next *Node       // make this `weak *Node` and the leak is gone
}

func main() {
    var a = new Node{Name: "a"}
    var b = new Node{Name: "b", Next: a}
    a.Next = b
    println(a.Next.Name, b.Next.Name)
}
```

`slang run --leak-check` prints:

```text
b a
leak: 2 objects
  2 x guide.debugging.leak.Node
  cycle through Node.Next (make one field of the cycle `weak *T` to break it)
```

## The debugger

Debug builds carry DWARF debug information. Three ways in:

```sh
slang debug run app.sl          # build with the in-process debug agent and debug it
slang debug app.sl              # build with DWARF and start lldb (or --gdb)
SLANG_DEBUG=:2345 ./app         # start the agent, then: slang debug attach :2345
```

`slang debug run` speaks gdb-style commands: `b file:line [if cond]`,
`b func`, `log file:line msg {expr}` (a logpoint), `c`, `n`, `s`, `o`
(step out), `p expr`, `locals`, `bt`, `up`/`down`, `tasks`, `task N`,
`catch panic`, `catch error [Type]`, `q`. Breakpoint conditions are
evaluated inside the process, so they are fast; `next` steps over an
`await` within the same task.

In VS Code, the extension in `editors/vscode` launches programs with the
agent (`"type": "slang"` in `launch.json`) and supports breakpoints,
conditions, logpoints, stepping, variables, tasks as threads, and
stopping on panics or on errors being created. With lldb or gdb, the
formatters in `tools/lldb` and `tools/gdb` show strings, slices, maps,
`T!`, enums and interfaces readably.

## Looking at what the compiler did

```sh
slang check --dump-hir app.sl        # the typed program after semantic analysis
slang build app.sl --emit=llvm-ir    # the LLVM IR
```

## Profiling

`std.diag` writes CPU profiles in pprof format, exposes Prometheus
metrics and records trace spans (exported to stdout or OTLP):

```slang
namespace guide.debugging.profile

import std
import std.io

func fib(n int) int {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}

func main() void! {
    var path = fs.Join(fs.TempDir(), "guide-cpu.pb")
    diag.StartCPUProfile(path)?
    var start = time.Now()
    var acc = 0
    for time.Since(start) < 200 {
        acc += fib(20)
    }
    var samples = diag.StopCPUProfile()?
    println("samples recorded:", samples > 0, acc > 0)
    fs.Remove(path)?
}
// Output:
// samples recorded: true true
```

Then `slang tool pprof -top guide-cpu.pb` (it runs `go tool pprof`)
shows where the time went. `diag.Runtime()` reports live objects,
allocations and scheduler state, and `diag.ServeMetrics` serves them with
your own counters at `/metrics`.
