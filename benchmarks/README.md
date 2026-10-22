# Benchmarks: Zell vs Go

Equivalent programs in `*/main.zl` and `*/main.go`. Run with
`./scripts/bench-perf.sh [name...]` (needs `target/release/zell` and `go`).
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
| `tcp_echo` | net: 1000 concurrent loopback TCP connections, 1 MB each written in 64 KB chunks and echoed back (`ReadInto` into per-connection buffers) |
| `http_hello` | net.http server answering a keep-alive load generator (64 connections, 200k `GET /` in all); `run.sh` cross-measures both servers with both load generators in separate processes, then HTTP/2 over TLS (`--server-tls` / `-server-tls`) with Go's h2 client (`-load2`) |
| `regex` | regex: `FindAll` of an e-mail pattern over a 10 MB generated text (lazy DFA with prefix acceleration vs Go's `regexp`) |
| `gzip` | compress.gzip: level-6 compression and decompression of 20 MB of mixed text, round trip checked |
| `alloc_heavy` | binary trees of small heap nodes (depth 4..16) and 4M rounds of non-escaping temporaries (`new Stats{}`, `&acc` passed to a method) |
| `heap_churn` | 20 rounds of 40 MB of small strings (two size classes) plus 32 x 256 KB buffers, all dropped each round; prints the RSS after the last round (memory returned to the OS) |
| `shared_objects` | reference counts across threads: one task builds 200k objects that 8 tasks read 10 times, 8 tasks build objects one task reads, 8 tasks churn task-local objects; prints the ms of each phase |

## Results, performance pass 2 (2026-10-21, 4 vCPU x86-64, clang/LLVM 18, go 1.24, `--release`)

Wall time, best of 5 runs with the three binaries of a benchmark run
alternately (`before` = the compiler and runtime of commit ec01e80, `after`
= this tree). The machine is shared, so differences under about 8% are
noise (map, struct_map: repeated runs give either one ahead).

| bench | before s | after s | go s | after / go |
|---|---|---|---|---|
| fib | 0.115 | 0.110 | 0.223 | 0.49 |
| sieve | 0.318 | 0.304 | 0.605 | 0.50 |
| map | 0.208 | 0.224 | 0.331 | 0.68 |
| strings | 0.225 | **0.072** | 0.199 | **0.36** |
| nbody | 0.473 | 0.458 | 0.481 | 0.95 |
| json_like | 0.225 | 0.213 | 0.306 | 0.70 |
| struct_map | 0.206 | 0.212 | 0.226 | 0.94 |
| alloc_heavy (new) | 1.119 | **0.398** | 0.598 | **0.67** |
| regex | 0.266 | 0.230 | 0.610 | 0.38 |
| gzip | 1.931 | 1.881 | 2.093 | 0.90 |

Networking (same session): `tcp_echo` 0.463 s before, 0.466 s after (epoll),
0.456 s with the io_uring backend, Go 0.540 s; `http_hello` with Go's load
generator, best of 3 rounds: 101 000 req/s before, 101 300 after (epoll),
104 500 (io_uring), Go 110 800 (rounds vary by ±8%).

Startup of `hello` (`--release`, 300 runs): 1.45 ms -> 1.08 ms (min), 1.81 ms
-> 1.30 ms (median). Sizes (`--release`, stripped): `hello` 35 584 -> 14 840
bytes (10 320 with `ZELL_LINKER=lld`), `http_server` 1 203 680 -> 1 232 968
(the stdlib grew in between), `web_app` 1 756 672 (the old compiler cannot
build today's `web_app`).

Compile time (`zell build`, best of 3, wall / CPU):

| program | before | after | after, 1 codegen thread |
|---|---|---|---|
| hello (debug) | 0.82 s | 0.65 s | 0.63 s |
| hello `--release` | 1.35 s | **0.31 s** | 0.30 s |
| http_server (debug) | 3.63 s | **1.94 s** (CPU 4.3 s) | 3.23 s |
| http_server `--release` | 11.6 s | 7.9 s (CPU 11.2 s) | 11.7 s |
| web_app (debug) | ~3.0 s (start of the session) | 2.62 s (CPU 5.5 s) | 4.08 s |
| web_app `--release` | ~9.8 s (start of the session) | 9.6 s (CPU 13.0 s) | 12.5 s |

A small debug build (a spec test) takes 0.72 -> 0.67 s. Building the whole
spec corpus 4 at a time took 133 s before and 159 s after on the shared
machine (load 4-6 from other jobs; the old compiler builds only 500 of the
820 files with today's stdlib, the new one 509), so that comparison is
inconclusive. `ZELL_TIME=1` prints the duration of each build phase.

What changed, by work item:

1. **Strings: views instead of copies** (runtime/ABI.md 4.1). `zell_string`
   is now `{hdr, len, char *data}` (32 bytes, owned bytes inline at +32), so
   a string can point into another string's bytes. `Split`/`Fields` put all
   their pieces into one *view block* (one allocation; each piece's refcount
   word forwards to the block), `s[a:b]`, `Trim*`, `SplitN` (and everything
   built on `s[a..b]` in Zell: `Cut`, regex match texts) return standalone
   views for pieces of 64 bytes or more and copies below. Views are not
   NUL-terminated: C APIs go through `zell_string_cstr`, and the runtime's
   test build `-DZELL_STRESS_NONUL` (no terminator on any string) passes the
   C tests, the examples and the spec corpus. `Split` with a one-byte
   separator counts and cuts 8 bytes at a time. `strings` 0.225 -> 0.072 s
   (Go 0.199 s). The other big part of that gain is general: object counts
   are per-thread counters instead of two shared `lock xadd` per allocation
   and one per free, and refcounts are updated without atomic instructions
   while the process has one thread (next item). JSON one-byte tokens are
   immortal statics. HTTP heads are parsed from a reused `[]byte` buffer, so
   they cannot be views (they were already copy-free C helpers).
2. **Reference counting fast paths.** The scheduler (workers and timer
   thread) now starts with the first task, so a program without tasks stays
   single-threaded; until a second thread starts (`zell_threaded`, set by
   the platform's thread start), `zell_retain`/`zell_release` use plain
   stores instead of atomic RMW. The compiler can emit that fast path inline
   (`codegen/rc.rs`), but only does so at `-O3` / `ZELL_INLINE_RC=1`: inline
   it gave json_like 7% and struct_map 2%, and grew `http_server` by 30% and
   its `--release` compile time by 70%.
3. **Escape analysis** (`hir::ownership::stack_locals`, DESIGN.md
   "Память"): an address-taken local whose address is only read through or
   passed to non-escaping parameters (escape facts that also count runtime
   calls as escaping), and `var p = new T{...}` whose `p` never escapes, live
   in an entry alloca with an immortal header; leaving the scope releases
   the fields (running `Drop`). alloc_heavy: 0.395 -> 0.301 s from escape
   analysis alone (`ZELL_NO_ESCAPE=1` turns it off), 1.12 -> 0.40 s in all
   (the binary trees part is allocator/refcount bound). nbody's only boxing
   (`for b in bodies { b.Energy() }`) runs 5 times, so it stays at parity: it
   is bound by `sqrt` and division latency.
4. **Interfaces and `any`.** Inside `case T x:` of a type switch on an
   interface local, calls through that local use `T`'s constant vtable and
   become direct (inlinable) calls. `any` holds bool/integers/floats/char
   inline in its data word (ABI.md 3.7): a loop boxing ints into `any` and
   switching on them went from 0.9 s to nothing (LLVM folds it).
5. **io_uring** (`runtime/src/platform/poll_uring.c`, Linux 6.0+, raw
   system calls): readiness polls (`IORING_OP_POLL_ADD`) with the epoll
   backend's one-shot level-triggered semantics, synchronous cancel on
   forget. Opt-in (`ZELL_IO_BACKEND=io_uring`): it is not faster than
   one-shot epoll here (one submission per wait, like one `epoll_ctl`), and
   the kernel drops a completed poll's file reference lazily, so a listening
   socket closed right after its poll completed may still accept briefly
   (seen in 2% of `examples/tcp_echo` runs). Multishot accept/recv was not
   attempted.
6. **Startup and size.** Lazy scheduler start, lazy `os.Args`, and
   shutdown/panic/leak-report/debug-agent hooks reached through function
   pointers set by the subsystems when they start, so `hello` links none of
   the I/O poller, resolver, debug agent or leak report code (text 24.6 KB
   -> 6.5 KB). `--gc-sections` already dropped unused stdlib code.
   `ZELL_LINKER=lld` saves another 4.5 KB but is not the default: its
   layout made fib 15% slower.
7. **Compile time.** The biggest hotspot was LLVM working on the whole
   stdlib: code generation emits every stdlib function a program could reach
   (1012 functions, 5.8 MB of IR for `hello`), and `-O2` spent a second on
   them before dropping them; a `globaldce` pass now runs first (`hello
   --release` 1.35 -> 0.31 s, debug object emission 465 -> 265 ms). Large
   modules (over 60k instructions after optimisation) are compiled to
   machine code on up to 8 threads (`compiler/src/parcodegen.rs`: the
   optimised module is split by function, each thread re-parses the bitcode
   and keeps its part, the objects are merged with `clang -r`;
   `ZELL_CODEGEN_THREADS=1` turns it off; it costs about 30% more CPU). The
   stdlib sources are parsed on several threads (168 -> 37 ms per build).
   Generic instantiation was not a hotspot. The 2x target is reached for
   debug builds of large programs (http_server 1.87x) and small release
   builds, not for large release builds (1.46x), where the serial `-O2`
   pipeline on the whole module dominates.

## Results (2026-10-20, x86-64, clang/LLVM 18, go 1.24, `--release`)

Wall time, best of 5 runs (`before` = commit 9b8d6b1 compiler + runtime,
`after` = this tree); Go measured in the same session. Peak RSS from
`./scripts/bench-perf.sh`.

| bench | zell before s | zell after s | go s | after / go | zell RSS MB | go RSS MB |
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
- **`zell_release`** skips the atomic RMW when the count is exactly 1 (the
  caller holds the only reference); retain stays a relaxed increment and the
  shared-count decrement `acq_rel`. Slice backings of `zell_ops_ref`
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
  hash, no indirect calls) and new `zell_map_get_or_insert_int/_str`, which
  the compiler now uses for `m[k] op= v`.
- **append** (`codegen/ops.rs`): `xs = append(xs, x)` stores the element and
  bumps `len` inline when `len < cap`; the runtime is called only to grow.
  `sieve` went 0.90 → 0.36 s (its fill loop was 50M runtime calls). The
  strided inner loop (`j += i`) cannot be vectorised by LLVM, but it is a
  plain load/compare/store with `len`/`data` reloaded (the slice variable's
  address escapes to the grow call, so LLVM cannot keep them in registers).
- **float/int formatting** (`runtime/src/string.c`): `zell__format_float`
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
| Zell | 0.52 | 1.64 | 57 | ~1.9 GB/s echoed |
| Go | 0.62 | 1.91 | 110 | ~1.6 GB/s echoed |

The kernel copies dominate both (sys time); only about 1 in 15 reads or
writes has to wait for readiness, so the poller round trip (epoll_ctl
one-shot arm, shard thread, run-queue post) is not the limit here. The
first version of the benchmark built its buffers with `bytes.Repeat`
(a Zell append loop: 196M appends, 5 s of user time); `net.MakeBuffer(n)`
/ `strings.Bytes(strings.Repeat(..))` allocate in one C call.

`http_hello` (`benchmarks/http_hello/run.sh`, requests/s, 64 keep-alive
connections, one request in flight each, server and load generator in
separate processes sharing the 4 vCPUs; the machine was shared with other
jobs, so the numbers move by ±15% between runs; two runs, the better one per cell):

| server \ load generator | Zell load | Go load | Go HTTP/2 load (TLS, 64 streams, 1 connection) |
|---|---|---|---|
| Zell `net.http` | 67 100 | 103 300 | 31 600 |
| Go `net/http` | 64 400 | 117 300 | 32 600 |

Six alternating rounds of the Go load generator against both servers
(`go -load`, same process pair): Zell 71–86k req/s, Go 61–85k req/s, ratio
98–117% (median about 110%), with the Zell server using 13–16 µs of CPU per
request (user + system) against Go's 15–17 µs. Before the rework of the HTTP
layer the Zell server did 51 700 req/s against Go's 104 300 (about half)
and used 35 µs of CPU per request. The HTTP/2 column is limited by the Go
client (its single h2 connection serializes the 64 streams' writes); both
servers stay below 100% CPU there.

What changed (a callgrind profile of the old server put ~90% of its
instructions in `http.zl`: `CanonicalHeader` alone 29%, `writeResponse` 22%,
the router 14%, and in task allocation of `async` helpers):

- Heads are parsed in place from the connection's read buffer by C helpers
  (`rtHeadEnd`, `rtParseHead`, ABI.md 16.5): no per-line strings, common
  methods/protocols/header names interned (no allocation), the facts the
  server needs (Content-Length, chunked, Connection, Expect, traceparent)
  returned as flags instead of map lookups.
- The response head (status line, headers, cached `Date`, Content-Length)
  and the body are formatted by one C call into one string and sent with one
  write (sendfile with TCP_CORK for files); `CanonicalHeader` returns its
  argument when it is already canonical.
- The connection loop does the reads itself (`conn.readInto` straight into
  the buffer): about three tasks per request (the read, the handler, the
  write) instead of ten; the router compares static routes as strings
  without splitting the path or allocating a parameter map.

HTTP/2 uses the same handler path with a task per stream; HPACK Huffman is
C, the rest of the framing Zell.

## Tier-2 stdlib (2026-10-20, `--release`, go 1.24)

The time inside the measured section, as each program prints it (best of 3);
the wall time from `bench-perf.sh` also includes generating the input.

| bench | zell ms | go ms | zell / go | note |
|---|---|---|---|---|
| regex `FindAll` (10 MB, 30323 matches) | 55 | 505 | 0.11 | Go's `regexp` has no DFA (NFA or backtracker); Zell runs a lazy DFA forward, then a reverse DFA for the start |
| gzip compress, level 6 (20 MB) | 1444 | 1705 | 0.85 | output 3 044 375 vs 3 022 734 bytes (+0.7%) |
| gzip decompress | 83 | 203 | 0.41 | table-driven inflate in C |

Peak RSS is higher for Zell (46 / 104 MB vs 27 / 76 MB): the inputs are
built by appending, and the gzip program keeps the input, output and
round-trip copy alive.


## Final results (2026-10-21, commit after the last wave, x86-64 Linux, clang/LLVM 18, go 1.24)

Best of several runs on an otherwise idle machine; `--release` for Zell, default build for Go.

| bench | Zell s | Go s | Zell/Go | Zell RSS | Go RSS |
|---|---|---|---|---|---|
| strings (Split/ToUpper/Join, 1.2 MB) | 0.07 | 0.19 | **0.37x** | 23 MB | 21 MB |
| regex (FindAll e-mails, 10 MB) | 0.21 | 0.58 | **0.36x** | 47 MB | 27 MB |
| fib(38) | 0.10 | 0.22 | **0.45x** | 11 MB | 11 MB |
| json_like (format 100k records) | 0.19 | 0.36 | **0.53x** | 23 MB | 27 MB |
| alloc_heavy (tree of small nodes) | 0.35 | 0.60 | **0.58x** | 11 MB | 14 MB |
| sieve (5 x 10M) | 0.28 | 0.46 | **0.61x** | 13 MB | 45 MB |
| map (4M int ops) | 0.19 | 0.30 | **0.63x** | 53 MB | 47 MB |
| tcp_echo (1000 conns x 1 MB) | 0.51 | 0.69 | **0.74x** | 55 MB | 109 MB |
| gzip (20 MB, level 6) | 1.68 | 2.03 | **0.83x** | 105 MB | 75 MB |
| nbody (5M steps) | 0.43 | 0.43 | 1.0x | 11 MB | 11 MB |
| struct_map | 0.23 | 0.22 | 1.0x | 12 MB | 13 MB |

HTTP (`benchmarks/http_hello/run.sh`, Go load generator, 64 keep-alive connections):

| server | HTTP/1.1 req/s | HTTP/2 over TLS req/s (64 streams) |
|---|---|---|
| Zell `net.http` | **116,507** | 26,290 |
| Go `net/http` | 101,979 | 29,389 |

The Zell load generator itself is slower than Go's (76k vs 117k req/s against the same server): client-side request building is the next thing to optimise.

Hello-world binary sizes (`scripts/bench-size.sh`, stripped release):

| target | size |
|---|---|
| Zell linux-amd64 (dynamic libc) | 14 KB |
| Zell linux-arm64 | 13 KB |
| Zell linux-musl (fully static) | 58 KB |
| Zell windows-amd64 | 68 KB |
| C (dynamic libc) | 15 KB |
| Go (stripped) | 1404 KB |

Summary: Zell is faster than Go on 9 of 11 CPU/memory benchmarks and on the HTTP/1.1 server, at parity on nbody and struct_map, and slightly behind on HTTP/2 over TLS. Memory use is lower or equal everywhere except regex (lazy DFA cache), gzip and map.

## Cycle collector (2026-10-21, 4 vCPU x86-64, `--release`, 10 alternating runs, wall seconds min / median)

`base` is the previous commit; `off`/`auto` are the same binary with
`ZELL_CYCLES=off` / the default `auto` (DESIGN.md "Циклы"). `base` vs
`off` also contains the concurrent allocator/biased-refcount work, so the
collector's own cost is `auto` vs `off`.

| Benchmark | base | off | auto | auto vs off |
|---|---:|---:|---:|---:|
| `json_like` | 0.180 / 0.199 | 0.172 / 0.185 | 0.172 / 0.186 | noise (no cyclic user types) |
| `http_hello` | 3.006 / 3.123 | 3.051 / 3.207 | 2.975 / 3.168 | noise (net.http has a few cyclic types: pools, h2 connections) |
| `alloc_heavy` | 0.362 / 0.402 | 0.444 / 0.499 | 0.415 / 0.446 | noise; callgrind (depth-12 run) 233M vs 224M instructions, +4 % |

`alloc_heavy`'s `Node {Left, Right *Node}` is a cyclic type (W0001 warns,
and it is deliberately not `@acyclic`), so all 14.7M nodes are flagged.
The first version recorded each of them as a possible root (the pin taken
around `check(n.Left)` is released to a non-zero count) and kept a hidden
buffer-index word per flagged object: +35..40 % (327M vs 209M
instructions). Pins of `*T` of cyclic types now remember the collection
epoch and are released with `zell_ptr_release_noroot`, which records
nothing unless a collection overlapped the pin (0 candidates in this
benchmark), the hidden word is gone, and the death of a flagged object
that was never recorded takes the plain drop + free path. RSS is equal
(11 MB). Binary size: `hello` 33 KB (15 KB at `base`; none of it is the
collector, which is linked through weak symbols only when a cyclic type
is allocated), `http_hello` 1.14 MB vs 1.11 MB.

## Object heap, biased refcounts, io_uring, testing.Panics (2026-10-21, 4 vCPU x86-64 Firecracker VM, Linux 6.18, `--release`)

`old` = the compiler and runtime of commit 5cbb7c9, `new` = this tree.
The machine was shared with another agent's test runs (load average 2 to
24 during these measurements), so wall times move by ±10-15% between
sessions; where they disagree, the callgrind instruction counts
(deterministic) and CPU time per request decide. `./scripts/bench-perf.sh`
now takes `RUNS=N` (best of N, Zell and Go alternating) and `NOGO=1`.

**1. Returning memory to the OS** (`runtime/src/heap.c`, ABI.md 3.2).
RSS (MB, `/proc/self/statm`) of a program that allocates 250 MB in 1 MB
`[]byte` buffers and 250 MB in 2.2M 80-byte strings (112-byte blocks),
then drops them and sleeps (the spec test `memory2/heap_release_rss`
checks the same with a 1 s limit):

| | start | allocated | freed +0 ms | +200 ms | +400 ms | +1000 ms |
|---|---:|---:|---:|---:|---:|---:|
| old | 1 | 540 | 271 | 271 | 271 | 271 |
| new | 1 | 540 | 352 | 352 | 18 | 18 |

(old: the 1 MB buffers came from glibc's mmap and went back; the
128-byte slab regions never did. new: the freed pages stay as reserve for
0.3 s, then the scavenger gives everything above 16 MB back;
`runtime.FreeOSMemory()` does it at once. Allocating the 540 MB took
1.94 s new vs 2.31 s old.)

Best of 5, alternating, `new` built from this tree (`alloc_heavy` with
`ZELL_CYCLES=off`, so the cycle collector's own cost is excluded; see the
previous section), wall s and peak RSS MB:

| bench | old s | new s | old MB | new MB | instructions old → new |
|---|---:|---:|---:|---:|---|
| fib | 0.097 | 0.112 | 11.2 | 11.2 | 1.343G → 1.343G (identical code: the wall time is noise) |
| sieve | 0.286 | 0.303 | 13.1 | 12.9 | 1.829G → 1.822G |
| map | 0.223 | 0.183 | 52.8 | 52.9 | 615M → 615M |
| strings | 0.065 | 0.065 | 23.3 | 23.7 | 719M → 711M |
| json_like | 0.180 | 0.168 | 23.3 | 24.8 | 1.651G → 1.701G (+3%; page faults 17.3k → 6.7k) |
| struct_map | 0.203 | 0.198 | 12.1 | 12.4 | 914M → 913M |
| alloc_heavy | 0.387 | 0.435 | 11.5 | 11.5 | 4.158G → 4.751G, of which 0.37G is `zell_cycles_new*` and the rest the collector's codegen; the heap's own alloc + free paths are 2.6G in both |
| regex | 0.207 | 0.209 | 46.6 | 51.0 | |
| gzip | 1.694 | 1.752 | 104.7 | 113.3 | |
| heap_churn (new) | 2.290 | 2.113 | 42.1 | 46.0 | RSS after the last round: 19 MB old (glibc trimmed the 132-byte strings) vs 43 MB new (one round's worth kept until the scavenger runs) |

Design notes and tuning: a first version kept a reserve of max(16 MB,
in use / 4) and released the rest synchronously; heap_churn then did 6.5k
`madvise` calls and re-faulted 40 MB per round (2.80 vs 2.34 s). Keeping
everything up to the recent peak synchronously made gzip's RSS 173 MB (vs
105): the reserve is now bounded by *peak − in use*, so in use + reserve
stays within the recent peak, and a background scavenger (a thread that
never touches objects, so `zell_threaded` stays 0) releases what stayed
unused for 0.3 s beyond 16 MB. The allocation fast path is inlined into
`zell_object_new` from `heap.h` (out of line it cost 18% more
instructions on alloc_heavy). Medium objects (16-128 KB) stay with malloc
(a mapping per 20 KB string would hit the 65k VMA limit); larger ones get a
mapping each, grown by `mremap`, with freed mappings cached within the
reserve.

**2. Biased reference counting** (`runtime/src/rc.c`, ABI.md 3.1).
`http_hello`, Go's load generator against the Zell server in another
process, best req/s of 4-5 rounds and the server's CPU time per request
(user + sys, minimum over the rounds):

| server | load ~5 | | load ~20 | |
|---|---:|---:|---:|---:|
| old | 101 536 req/s | 16.4 µs | 74 585 req/s | 15.7 µs |
| new (biased) | 98 440 req/s | 16.2 µs | 81 996 req/s | 16.2 µs |
| new, `ZELL_NO_BIAS=1` | 100 272 req/s | 16.4 µs | 84 032 req/s | 15.5 µs |

Before tasks were excluded from biasing (`ZELL_RC_NOBIAS`), the server
used 19.9 µs per request (vs 18.6 without biasing): with `ZELL_RC_STATS=1`
the in-process benchmark queued 1.8M objects for a merge (9 per request,
91% of them `Task` objects, which every await hands to another thread);
with tasks never biased it is 183k (5.6M -> 2.4M objects biased).

`shared_objects` (new), ms per phase (one builder / 8 readers, 8 builders
/ one reader, 8 tasks with local churn), minimum of 10 alternating runs:

| | phase 1 | phase 2 | phase 3 |
|---|---:|---:|---:|
| old (load ~5) | 426 | 125 | 124 |
| new (load ~5) | 350 | 104 | 107 |
| new, `ZELL_NO_BIAS=1` (load ~5) | 451 | 147 | 131 |
| old (load ~20) | 437 | 137 | 140 |
| new (load ~20) | 387 | 146 | 132 |
| new, `ZELL_NO_BIAS=1` (load ~20) | 314 | 144 | 129 |
| Go | 60-69 | 23-39 | 124-196 |

The quieter session shows biasing 15-30% ahead in every phase, the loaded
one does not separate them: a win on the churn phase (owner-local counts
are plain stores) is plausible but not established beyond the noise here.
Phase 1 is bound by the shared counts of the 200k objects every reader
touches (cache-line ping-pong; Go has no counts). Single-threaded
programs never bias (their instruction counts above are unchanged).
`tcp_echo` (`ZELL_RC_STATS`: 93k biased, 24k queued) is within noise.
`hello` grew from 15 KB to 33 KB (the heap, the scavenger and rc.c are
reachable from `zell_release`).

**3. io_uring** (`ZELL_IO_BACKEND=io_uring`): `examples/tcp_echo` gave 0
wrong outputs in 500 runs before and after the fix (the 2% flake was seen
under gVisor; the race — a completed poll's file reference dropped just
after its CQE is posted — remains possible, so a listener is now shut down
before close, which stops it accepting whatever still holds the file).
Performance: `tcp_echo` best 0.431 s vs 0.456 s for epoll (median 0.536 vs
0.558, 12 alternating runs each): parity; `http_hello` 66-73k vs 82-91k
req/s and 20.6 vs 16.2 µs of server CPU per request: about 20% slower (a
POLL_ADD submission per wait plus the completion reaped under the ring
lock; submitting outside the lock did not help). So it stays opt-in.

**4. testing.Panics** runs the function on a thread of its own and catches
its panic (setjmp/longjmp), so the test's code runs once instead of once
more per `Panics` call in a child process; `compiler/tests/zell_test_cmd.rs`
covers both paths (`ZELL_TEST_PANICS=process` forces the old one).

## Memory wave results (2026-10-21, after cycle collector, OS memory return, biased RC)

Best of 3 on an idle machine, `--release`, cycle collector in default `auto` mode.

| bench | Zell s | Go s | Zell/Go | Zell RSS | Go RSS |
|---|---|---|---|---|---|
| strings | 0.069 | 0.167 | **0.41x** | 24 MB | 18 MB |
| regex | 0.234 | 0.588 | **0.40x** | 51 MB | 24 MB |
| sieve | 0.324 | 0.608 | **0.53x** | 13 MB | 43 MB |
| fib | 0.115 | 0.213 | **0.54x** | 11 MB | 11 MB |
| json_like | 0.180 | 0.293 | **0.61x** | 25 MB | 34 MB |
| map | 0.242 | 0.386 | **0.63x** | 53 MB | 44 MB |
| alloc_heavy | 0.448 | 0.561 | **0.80x** | 11 MB | 14 MB |
| struct_map | 0.196 | 0.227 | **0.86x** | 12 MB | 13 MB |
| tcp_echo | 0.482 | 0.553 | **0.87x** | 56 MB | 117 MB |
| gzip | 1.910 | 2.101 | **0.91x** | 113 MB | 75 MB |
| nbody | 0.430 | 0.446 | 0.96x | 11 MB | 11 MB |
| heap_churn (new) | 2.244 | 0.976 | 2.3x slower | 46 MB | 67 MB |
| shared_objects (new) | 0.636 | 0.217 | 2.9x slower | 47 MB | 20 MB |

Regressions and open items from this wave:

- `heap_churn` (allocate/free 1 MB buffers and small strings in a loop) and
  `shared_objects` (objects created on one task, read from 8 workers) are
  the two benchmarks where Go's allocator and GC beat ARC: large-buffer
  churn pays mmap/munmap and page-fault costs, and cross-thread reads pay
  atomic shared counts even with biased RC. Both are new benchmarks added
  to make this visible; they are the next optimisation targets (large-object
  cache sizing, read-only borrows without retain across tasks).
- Release `hello` grew from 14 KB to 32 KB: the segmented heap, the
  scavenger and `rc.c` are reachable from `zell_release`.
- `alloc_heavy` with the collector on is within noise of `ZELL_CYCLES=off`
  after the pin-release optimisation; the W0001 warning it prints is
  expected (`Node` is a tree, but the type can form a cycle).

Stdlib benchmarks (`json_codec`, `encodings`, `crypto`, `file_io`, `sort_collect`): see [RESULTS-stdlib.md](RESULTS-stdlib.md).
