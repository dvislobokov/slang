# Latency, scaling and language mechanisms (preliminary)

Three Zell vs Go benchmarks for questions the throughput suite in
`README.md` does not answer: tail latency under allocation pressure (the
ARC-vs-GC pause argument), scaling over cores, and the cost of individual
language mechanisms.

**Status: programs only, no results yet.** The machine (4 vCPU) was shared
by five agents when they were written, so nothing was timed. Both sides of
each benchmark were built and run once at `SCALE=small`, and their
checksums are equal. The results tables below are TODO, to be filled from a
quiet run. The smoke runs printed some timings, but they come from tiny
workloads on a loaded machine and are not results. The only thing taken
from them is the open items at the end, and each of those also has evidence
from the IR or the heap counters.

## How to run

```sh
cargo build --release -p zell && make -C runtime
RUNS=5 ./scripts/bench-perf.sh latency_p99 cores_scaling defer_errors_iface
SCALE=small ./scripts/bench-perf.sh defer_errors_iface     # smoke test (~seconds)
benchmarks/latency_p99/bench.sh                             # one benchmark directly
```

`scripts/bench-perf.sh` now hands a benchmark that has an executable
`benchmarks/<b>/bench.sh` to that driver, passing `ZELL` and `RUNS`.
`SCALE` comes through the environment. Every driver builds `main.zl`
(`--release`) and `main.go`, runs them alternately `RUNS` times, fails if
the deterministic stdout of the two sides differs, and prints the best run.
Programs print checksums on stdout and timings on stderr. The timing lines
have the form `ms|us <row...> <value>`, and `scripts/bench-rows.py` turns
them into one row per measurement, with Zell/Go ratios. `SCALE=small`
shrinks every workload by about 100x and does not change the shape of a
benchmark.

For the quiet re-run: `RUNS=5`, nothing else running (`uptime` load < 0.5),
and the same machine for both languages. Go 1.24 runs with default `GOGC`
and `GOMAXPROCS`. Zell runs with default `ZELL_WORKERS` (online CPUs).

## latency_p99

An HTTP/1.1 server answers `GET /` with a 34-byte JSON body
(`{"message":"hello, world","id":1}`, `Content-Type: application/json`).
Each server is measured in two modes:

- **idle**: the server alone. This is the baseline.
- **churn**: the server plus one background task (Zell) or goroutine (Go)
  that keeps a large live heap and replaces it piece by piece as fast as
  it can. The heap has 1000 buckets. Each bucket holds a list of 3200
  objects (`{Id int, Name string, Prev *Obj}`, where the name is a small
  formatted string) and a map from id to every other object. One rebuild
  replaces the bucket an LCG picks, so 3200 objects, their strings and a
  1600-entry map become garbage (Go) or are freed at once (Zell). At full
  size the live heap is about 200 MB in Go (`HeapAlloc` after a GC: 203 MB)
  and 436 MB in Zell (`diag.rtHeapInUse`); see open item 4. The Zell task builds
  the new bucket in chunks of 400 objects and calls `await task.Yield()`
  after each chunk, because scheduling is cooperative (open item 6). The
  old bucket is freed when the new one replaces it in the slot. Go's
  goroutine builds the bucket in one go and is preempted by the runtime. Every second, the server prints its rebuild
  rate (`churn/s`) to stderr. The driver reports the average rate, so it
  is visible whether both sides really churned at a comparable rate.

The load generator is Go's, taken from `http_hello` (`main.go -load`): 64
keep-alive connections, 200k requests in all, one request in flight per
connection, after 100 warm-up requests. It records the latency of every
request (write to end of body) and reports req/s and p50/p90/p99/p999/max
in microseconds. Every body is checked byte for byte. The output
`requests N bytes B` must be equal for all four runs (zell/go x idle/churn).
The churn also has a deterministic self-check: `main` with no arguments
builds the heap, does 2000 rebuilds and prints object counts and
checksums, which must be equal on both sides (`bench.sh` checks this
first). `SCALE=small` uses 100 buckets of 150 objects and 20k requests.

Results (best of `RUNS` by p99; µs; Go = 1.0 in the ratio rows): **TODO
(quiet run)**

| server | mode | req/s | p50 | p90 | p99 | p999 | max | churn/s |
|---|---|---|---|---|---|---|---|---|
| zell | idle | | | | | | | - |
| go | idle | | | | | | | - |
| zell | churn | | | | | | | |
| go | churn | | | | | | | |
| zell/go | churn | | | | | | | |

What to look at: how much churn moves p99, p999 and max away from each
server's idle baseline. The absolute numbers matter less. Go's concurrent
GC has no long stop-the-world pause, but mark assists and the GC's 25% CPU
share land on request goroutines. Zell frees a whole bucket synchronously
on the churn thread. That is about 3200 releases, a cost paid by that
thread and not by the request handlers, unless they share its worker
(open item 6).

## cores_scaling

The total work is fixed and split among 1, 2 or 4 workers: Zell tasks
(`async`, first `await task.Yield()` so they run on the pool) and Go
goroutines.

- **wordfreq**: word frequencies over 400 MB of generated text. The text
  is one shared read-only `[]byte` of words drawn with a skewed LCG choice
  from a 1000-word vocabulary. It is split at spaces into one range per
  worker. Each worker counts FNV-1a hashes of words into its own
  `map[int]int`, and the maps are merged at the end. This phase is CPU
  bound (byte scanning plus map updates) and shares no writable memory.
- **trees**: 2048 binary trees of depth 14 (32767 nodes each, 67M
  allocations in all) built and checked, divided among the workers. Each
  worker also keeps one long-lived tree. This phase is allocation heavy
  and exposes allocator contention (Zell: per-thread heap and refcount
  fast paths; Go: per-P mcache plus GC).

The output gives counts and checksums per worker count, and they must not
depend on the worker count. The driver prints the best ms per row and the
1->4 speedup for each language. `SCALE=small` uses 4 MB of text and 32
trees.

Results (ms, best of `RUNS`): **TODO (quiet run)**

| phase | workers | zell | go | zell/go |
|---|---|---|---|---|
| wordfreq | 1 | | | |
| wordfreq | 2 | | | |
| wordfreq | 4 | | | |
| trees | 1 | | | |
| trees | 2 | | | |
| trees | 4 | | | |
| speedup 1->4 | wordfreq | | | |
| speedup 1->4 | trees | | | |

With 4 vCPU and a 4-worker run, any other load on the machine shows up
directly in the 4-worker numbers. That is the main reason this table must
come from a quiet run.

## defer_errors_iface

Each mechanism gets its own row, and each row is a loop whose result feeds
a checksum (µs, best of `RUNS`). `bench.sh` builds the Zell program twice:
with error return traces (the default) and with `--no-error-trace`.

| row | iterations | what |
|---|---|---|
| `direct_call` | 100M | method calls on concrete structs (two types, alternating) |
| `iface_call` | 100M | the same methods through an interface: `ops[i & 1].Apply(i)`, two implementations, one call site |
| `closure_call` | 100M | closures capturing a local, called through a `[]func(int) int` |
| `defer_call` | 20M | a function with `defer c.Inc()`, called in a loop |
| `no_defer_call` | 20M | the same function without `defer` |
| `error_new` | 20M | 1 in 8 calls fails: `error("odd")` created at the bottom of 3 levels of `?` (Go: `errors.New` + `if err != nil { return }`) |
| `error_sentinel` | 20M | the same with a constant error (`const ErrOdd = error(...)`; Go: a package `var`) |
| `generic_sum` | 100M adds | `SumG<T: Add>(xs []T)` over `[]int` (1M elements, 100 passes; Go: `SumG[T ~int\|~float64]`) |
| `iface_sum` | 100M calls | `SumI(xs []Valuer)` over boxed `*Val`, the interface-based equivalent |

Results (µs; Go = 1.0 in the ratio columns): **TODO (quiet run)**

| row | zell | zell --no-error-trace | go | zell/go | notrace/go |
|---|---|---|---|---|---|
| direct_call | | | | | |
| iface_call | | | | | |
| closure_call | | | | | |
| defer_call | | | | | |
| no_defer_call | | | | | |
| error_new | | | | | |
| error_sentinel | | | | | |
| generic_sum | | | | | |
| iface_sum | | | | | |

How to read the rows: LLVM vectorizes the `direct_call`, `no_defer_call`
and `generic_sum` loops in Zell (`llvm.vector.reduce.add` in the IR), and
Go does not. Those rows therefore measure "the call disappears entirely"
and not the cost of a call. Compare `iface_call` and `closure_call` with
Go's equivalents, not with `direct_call`. Go has no error traces, so the
fair comparisons are `zell --no-error-trace` vs Go for the cost of the
mechanism, and `zell` vs `zell --no-error-trace` for the cost of traces.

## Open items (found while writing; for the compiler/runtime owners)

These come from the IR (`zell build --emit=llvm-ir`) and the heap counters.
The smoke-run hints are from a loaded machine and still need confirming on
a quiet run.

1. **`defer` allocates on every execution.** In `withDefer`, `defer
   c.Inc()` becomes two `zell_object_new` per call (a bound-method value
   capturing `c`, type `ti.24`, and a closure env `ti.env`), plus 3
   retain/release pairs and 2 frees. Go open-codes such defers to a few
   instructions. A defer of a direct call with already-evaluated arguments
   could store the receiver and arguments in the frame, with no closure
   (Go-style open-coded defers). The smoke run hinted at >10x Go.
2. **Retain/release of a borrowed receiver around interface and closure
   calls.** `ops[i & 1].Apply(i)` emits `zell_ptr_retain(recv)`,
   `call vtable[..]`, `zell_ptr_release(recv)` per call, both out of line.
   The same happens for `fs[i & 1](i)` (the closure env) and for `for x in
   xs { x.Value() }` over `[]Valuer`. The slice keeps the element alive for
   the whole call. The callee could also be handed a borrowed receiver (it
   is `readonly` already) when the element cannot be overwritten during the
   call (no stores to the slice, or a callee without side effects on it).
   The smoke run hinted at about 4x Go for `iface_call` and `iface_sum`.
3. **Sentinel errors with traces allocate per failing return.** `return
   ErrOdd` and every `?` go through `zell_error_returned`. Because the
   constant is always shared, copy-on-write (DESIGN.md "Трасса ошибки")
   copies it on the first `return`, so every failing call allocates even
   though the error is a constant. With `--no-error-trace` the row is
   close to Go. Options: a trace record kept beside the error value (in
   the `T!` result slot) instead of inside the shared object, or one
   allocation per error for its whole path with no copy on later `?`.
4. **Memory per small object is about 2x Go's.** The same 3.2M-object
   heap (latency_p99 self-check, full size) has Zell `diag.rtHeapInUse` at
   436 MB and Go `HeapAlloc` after a GC at 203 MB. Peak RSS is 814 MB vs
   434 MB (Go's includes the GOGC=100 headroom, Zell's has no such
   reason). That is about 136 B vs 63 B per object, counting its string,
   list slot and half a map entry. Zell's RSS is also 1.9x its own heap in
   use after the build. Worth a `diag.HeapByType()` breakdown: object
   header size, small-string layout (`zell_string` is 32 bytes + bytes),
   map entry size, and pages retained from `append` growth.
5. **Self-referencing types are cycle-collector candidates.** `Obj.Prev
   *Obj` (latency_p99) and `Node.Left/Right` (cores_scaling) trigger
   W0001. The objects form lists and trees, never cycles, but the types
   could form them, so the bounded cycle collector tracks them. The
   benchmarks keep them this way on purpose, since realistic code looks
   like this. A quiet run with `ZELL_CYCLES=off` (as in `alloc_heavy`)
   gives the cost of that tracking under churn and across threads.
6. **Cooperative scheduling and CPU-bound tasks.** The churn task has to
   `await task.Yield()` every 400 objects. A bucket rebuild without yields
   (3200 objects) keeps its worker thread for about 2 ms (scaled from the
   smoke run's rebuild rate). A request task queued on that worker waits
   that long, because Zell has no preemption (Go has async preemption
   since 1.14). With the chunks, the wait is a chunk, plus the synchronous
   release of the old bucket (3200 objects + map). On a quiet run, check
   whether work stealing moves queued tasks off a busy worker fast enough
   to keep this out of p999. A "CPU-bound/blocking task" API
   (`task.SpawnBlocking` or worker affinity) would let programs keep
   background CPU work off the I/O workers, and freeing a large structure
   could be deferred in pieces (a release queue drained between tasks).
7. **No per-program worker count.** cores_scaling varies the number of
   tasks while the pool stays at `ZELL_WORKERS` (default 4), as with Go
   goroutines under `GOMAXPROCS`. A run with `ZELL_WORKERS=1` /
   `GOMAXPROCS=1` and 4 workers would separate "parallel speedup" from
   "task overhead". The drivers do not do that yet.
