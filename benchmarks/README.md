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
| `json_like` | string building: `fmt.Format` of 100k JSON-like records (ints, floats, bools) + `Join`, 5 rounds |
| `struct_map` | map[string]Point insert/update/lookup (100k keys, 2M updates, 100k lookups) |

## Results (2026-10-20, x86-64, clang/LLVM 18, go 1.24, `--release`)

Wall time, best of 5 runs (`before` = commit 9b8d6b1 compiler + runtime,
`after` = this tree); Go measured in the same session. Peak RSS from
`./scripts/bench-perf.sh`.

| bench | slang before s | slang after s | go s | after / go | slang RSS MB | go RSS MB |
|---|---|---|---|---|---|---|
| fib | 0.11 | 0.13 | 0.23 | 0.57 | 10.9 | 10.8 |
| map | 0.64 | 0.29 | 0.52 | 0.56 | 53.0 | 45.3 |
| nbody | 0.47 | 0.47 | 0.48 | 0.97 | 10.8 | 10.9 |
| sieve | 0.90 | 0.36 | 0.59 | 0.61 | 13.1 | 42.4 |
| strings | 0.69 | 0.22 | 0.27 | 0.80 | 20.4 | 20.4 |
| json_like | 1.24 | 0.23 | 0.37 | 0.62 | 23.4 | 25.9 |
| struct_map | 0.38 | 0.21 | 0.25 | 0.86 | 12.3 | 14.2 |

What changed (runtime, plus codegen fast paths):

- **Small-object allocator** (`runtime/src/mem.c`): objects up to 128 bytes
  come from per-thread free lists (16-byte classes, 4 MB regions, a bitmap
  identifies region memory on free) instead of `calloc`/`free`. This alone
  took `strings` from ~0.6 s to ~0.2 s: `Split` makes 200k small strings per
  round and glibc's malloc spent most of its time consolidating. Strings are
  allocated uninitialised (only `len` and the NUL are written).
- **`slang_release`** skips the atomic RMW when the count is exactly 1 (the
  caller holds the only reference); retain stays a relaxed increment and the
  shared-count decrement `acq_rel`. Slice backings of `slang_ops_ref`
  elements release their elements without an indirect call per element.
- **strings**: `Split`/`Count`/`Replace`/`Index`/`Contains` search with
  `memchr` + `memcmp` instead of a `memcmp` at every byte; `Split` counts
  first and fills the result slice once; `ToUpper`/`ToLower` write straight
  into the result string with an 8-bytes-at-a-time ASCII path (no temporary
  buffer).
- **map** (`runtime/src/map.c`): the chained table with one `malloc` per
  entry and function-pointer hash/eq became a SwissTable-style open-addressing
  table (control bytes, SWAR group matching, 7/8 load) with entries inline,
  specialised int and string key paths (inlined mixer / word-at-a-time string
  hash, no indirect calls) and new `slang_map_get_or_insert_int/_str`, which
  the compiler now uses for `m[k] op= v`.
- **append** (`codegen/ops.rs`): `xs = append(xs, x)` stores the element and
  bumps `len` inline when `len < cap`; the runtime is called only to grow.
  `sieve` went 0.90 → 0.36 s (its fill loop was 50M runtime calls). The
  strided inner loop (`j += i`) cannot be vectorised by LLVM, but it is a
  plain load/compare/store with `len`/`data` reloaded (the slice variable's
  address escapes to the grow call, so LLVM cannot keep them in registers).
- **float/int formatting** (`runtime/src/string.c`): `slang__format_float`
  tries an exact fast path for fixed-notation values (1e-4 <= |v| < 1e6, at
  most 15 digits) before the `%.*e` + `strtod` search, and ints are formatted
  without `snprintf`. `json_like` went 1.24 → 0.23 s (79% of its time was
  float formatting). Verified identical output against the old algorithm on
  6M random values.
- **nbody**: the hot loop was already a direct GEP + load/store with no
  refcount traffic; `math.Sqrt/Abs/Floor/Ceil` now lower to LLVM intrinsics
  (inline `sqrtsd`) instead of runtime calls. It stays at parity, bound by
  the `sqrt` + divide latency.
- `fib` emits identical IR before and after; its ±15 ms difference is
  code layout (it flips sign between the stripped and unstripped builds).

Notes on the first measurement (2026-10-20, naive ARC, before the changes
above): fib 0.15 / nbody 0.59 / sieve 2.23 / map 0.94 / strings 0.70 s
against Go 0.26 / 0.51 / 0.77 / 0.53 / 0.24 s; `sieve` and `map` paid a
runtime call per element operation, `strings` per-element `malloc` traffic.
