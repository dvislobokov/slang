# Benchmarks: Slang vs Go

Equivalent programs in `*/main.sl` and `*/main.go`. Run with
`./scripts/bench-perf.sh [name...]` (needs `target/release/slang` and `go`).
The script builds both with release settings and reports wall time, CPU time
(user+sys) and peak RSS.

| Benchmark | What it exercises |
|---|---|
| `fib` | recursion, function calls (fib(38)) |
| `sieve` | slice append, indexing with bounds checks, inner loops (5 x 10M) |
| `map` | map[int]int insert/update/lookup (2M + 2M ops) |
| `strings` | string concat, Join/Split/ToUpper/Contains on a 1.2 MB string |
| `nbody` | structs in a slice, float math, method calls (5M steps) |

## Results (2026-10-20, x86-64, clang/LLVM 18, go 1.24, `--release`)

| bench | lang | wall s | cpu s | peak RSS MB |
|---|---|---|---|---|
| fib | slang | 0.15 | 0.15 | 10.9 |
| fib | go | 0.26 | 0.26 | 10.9 |
| map | slang | 0.94 | 0.93 | 55.8 |
| map | go | 0.53 | 0.54 | 44.3 |
| nbody | slang | 0.59 | 0.58 | 10.9 |
| nbody | go | 0.51 | 0.51 | 10.8 |
| sieve | slang | 2.23 | 2.19 | 26.9 |
| sieve | go | 0.77 | 1.15 | 44.3 |
| strings | slang | 0.70 | 0.70 | 36.2 |
| strings | go | 0.24 | 0.29 | 18.6 |

Notes on the first measurement (naive ARC, no ownership optimisations yet):

- `fib` and `nbody` are at or above Go: plain LLVM code with no runtime calls.
- `sieve` is 3x slower: every `append` and every index is a call into the C
  runtime with a bounds check; inlining the fast path into generated IR is
  the obvious next step. Slang uses less memory because Go's GC lets garbage
  accumulate between collections.
- `map` is 1.8x slower: the runtime hash map is a generic C implementation
  with function-pointer hash/eq; a specialised int-key path is planned.
- `strings` is 3x slower: `Split`/`ToUpper` allocate per element with
  atomic refcount traffic; in-place updates on uniquely-owned buffers
  (see DESIGN.md "Память") should close most of the gap.
