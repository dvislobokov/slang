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
| `tcp_echo` | std.net: 1000 concurrent loopback TCP connections, 1 MB each written in 64 KB chunks and echoed back (`ReadInto` into per-connection buffers) |
| `http_hello` | std.net.http server answering a keep-alive load generator (64 connections, 200k `GET /` in all); `run.sh` cross-measures both servers with both load generators in separate processes |
| `regex` | std.regex: `FindAll` of an e-mail pattern over a 10 MB generated text (lazy DFA with prefix acceleration vs Go's `regexp`) |
| `gzip` | std.compress.gzip: level-6 compression and decompression of 20 MB of mixed text, round trip checked |

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

## Networking (2026-10-20, 4 vCPU x86-64 gVisor sandbox, loopback, go 1.24)

`tcp_echo` (`./scripts/bench-perf.sh tcp_echo` measures one run; the table is the best of 5, wall time for
1000 connections x 1 MB echoed = 2 GB through the kernel):

| | wall s | CPU s (user+sys) | peak RSS MB | throughput |
|---|---|---|---|---|
| Slang | 0.52 | 1.64 | 57 | ~1.9 GB/s echoed |
| Go | 0.62 | 1.91 | 110 | ~1.6 GB/s echoed |

The kernel copies dominate both (sys time); only about 1 in 15 reads or
writes has to wait for readiness, so the poller round trip (epoll_ctl
one-shot arm, shard thread, run-queue post) is not the limit here. The
first version of the benchmark built its buffers with `std.bytes.Repeat`
(a Slang append loop: 196M appends, 5 s of user time); `net.MakeBuffer(n)`
/ `strings.Bytes(strings.Repeat(..))` allocate in one C call.

`http_hello` (`benchmarks/http_hello/run.sh`, requests/s, 64 keep-alive
connections, one request in flight each, server and load generator in
separate processes sharing the 4 vCPUs; two runs, the better one):

| server \ load generator | Slang load | Go load |
|---|---|---|
| Slang `std.net.http` | 43 900 | 51 700 |
| Go `net/http` | 59 400 | 104 300 |

The Slang server reaches about half of Go's `net/http`. A callgrind
profile of the in-process run puts most of the server's time in the
Slang-source HTTP layer (`http.sl`: header canonicalisation, splitting,
string building) and in per-call task allocation of `async` functions;
the socket layer itself (`sock.read/readInto/waitRead`) is a few percent.
Fixed on the way: `Conn.Read(n)` used to allocate and zero an n-byte slice
per call (16 KB per HTTP read, 24% of all instructions in memset); it now
receives into a per-thread 64 KB buffer and copies exactly the bytes
received. Each wait still costs a hop from the shard's poller thread to a
worker through the global run queue (Go polls inside its workers): that is
the next step if the HTTP layer stops dominating (ABI.md 16.1).

## Tier-2 stdlib (2026-10-20, `--release`, go 1.24)

The time inside the measured section, as each program prints it (best of 3);
the wall time from `bench-perf.sh` also includes generating the input.

| bench | slang ms | go ms | slang / go | note |
|---|---|---|---|---|
| regex `FindAll` (10 MB, 30323 matches) | 55 | 505 | 0.11 | Go's `regexp` has no DFA (NFA or backtracker); Slang runs a lazy DFA forward, then a reverse DFA for the start |
| gzip compress, level 6 (20 MB) | 1444 | 1705 | 0.85 | output 3 044 375 vs 3 022 734 bytes (+0.7%) |
| gzip decompress | 83 | 203 | 0.41 | table-driven inflate in C |

Peak RSS is higher for Slang (46 / 104 MB vs 27 / 76 MB): the inputs are
built by appending, and the gzip program keeps the input, output and
round-trip copy alive.

