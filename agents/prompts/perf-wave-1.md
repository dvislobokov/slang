# Prompt: performance wave 1 (five hot spots)

Run on a machine with 8+ cores and 60+ GB of free disk (each parallel agent
needs its own git worktree and `target/`). Start `claude` in the repository
root (`master` at or after commit 76b5ff8) and paste the prompt below.

Measurements it refers to: `benchmarks/README.md` (summary, "Async
benchmarks", "Performance pass 3"), `benchmarks/RESULTS-latency.md`,
`RESULTS-stdlib.md`, `RESULTS-framework.md` (quiet run of 2026-10-22,
Go = 1.00).

---

Read CLAUDE.md, DESIGN.md, runtime/ABI.md, benchmarks/README.md and the three
benchmarks/RESULTS-*.md files. The quiet benchmark run found five hot spots
where Zell is far behind Go. Fix them with five parallel Opus subagents,
each in its own git worktree (`isolation: worktree`), each owning disjoint
paths, each measuring only its own benchmarks (`RUNS=3`, `SCALE` as needed;
no full-suite runs). After all five report, merge their branches into
master in this order (scheduler, io, json, crypto, compiler), resolve
conflicts, run `make -C runtime test`, `make -C runtime asan`,
`cargo build --workspace` (warning-free) and the full `cargo test
--workspace`, then run the affected benchmarks once more on the merged
tree, record before/after tables (Go = 1.00) in a new section
"Performance wave 1" of benchmarks/README.md, commit on master in logical
commits and push to `https://github.com/zell-lang/zell`.

Subagent 1, **scheduler** (owns `runtime/src/task.c`, `task_io.c`,
`runtime/tests/test_task.c`, `std/task/**`, `compiler/src/sema/stdlib_src/task*.zl`,
plus whatever `select` needs in `compiler/src` and `grammar/zell.bnf`,
DESIGN.md "Асинхронность"):
- per-worker run queues with work stealing and a "run next" slot (the
  resumed continuation of an await runs on the current worker without
  going through a global queue); today one global queue under a mutex costs
  ~780 ns per resumed frame (await_chain 125x Go, async_io 0.27x);
- `task.Channel` in C (`task.c`) instead of Zell-over-signals: a recv wakes
  exactly one blocked sender and vice versa (fan-in 10k producers wakes all
  10k today; ping-pong 35x Go, pipeline 11x);
- `task.Mutex`: uncontended lock/unlock without allocating a task (4.7x);
  contended path without a FIFO convoy through the scheduler (33x);
- an async call that completes without suspending must not allocate a task
  or a frame (136 ns vs 8 ns per call): synchronous fast path, ABI change
  documented in runtime/ABI.md and compiler/src/codegen/mod.rs together;
- `select` over channels and timers (Go semantics: random fair choice,
  `default`), specified in DESIGN.md + EBNF + grammar/zell.bnf, with spec
  tests and an example; replace the `task.Any` emulation in
  benchmarks/select_many;
- targets: channel_pingpong ≤ 3x Go, worker pool ≤ 3x, mutex ≤ 3x,
  await_chain ≤ 5x, async_io ≥ 0.6x Go round trips, select ≤ 3x; `make
  tsan` green; all `--leak-check` examples green.

Subagent 2, **I/O** (owns `std/io/**`, `std/bufio/**` if present,
`std/strings/**`, `runtime/src/io.c`, `runtime/src/std_strings.c`,
`runtime/src/std_io*.c`):
- buffered reader/writer with 1 KB calls: 9.3x / 2.4x Go. Reads must copy
  straight from the buffer into the caller's slice, no intermediate
  strings; line scanning with `memchr` over the buffer (2.9x);
- `strings.Builder` with one growing buffer instead of keeping every piece
  (2.1x; 30M parts for 100 MB);
- `io.Copy` with `copy_file_range`/`sendfile` on Linux when both ends are
  files or sockets (0.83x);
- 1 MB-chunk write at 0.71x: find the extra copy;
- targets: all file_io rows ≥ 0.8x Go, strings.Builder ≥ 0.8x.

Subagent 3, **JSON** (owns `runtime/src/std_json.c`, `std/encoding/json/**`,
`compiler/src/sema/stdlib_src/json*.zl`, the JSON parts of
`compiler/src/sema/reflect.rs` if `Parse<T>` is generated there):
- the tokenizer allocates and zeroes a buffer the size of the rest of the
  document for every string token (`scan_string`): 497x Go on 8 MB. Scan
  once, unescape in place or into a right-sized buffer;
- `Parse<T>` makes three passes and builds error-path strings for every
  field before anything fails: one pass, lazy error paths;
- `json.Format` writes `null` for an empty slice: write `[]` (Go
  semantics: only a nil slice is `null`; decide and document in
  examples/README.md);
- Value parse / format / lookups (463x / 2.3x / 5.7x), 1M small messages
  (0.37x);
- targets: typed parse and Value parse ≤ 1.5x Go, format ≤ 1.5x, small
  messages ≥ 0.7x; `json_codec` at full size finishes in < 30 s.

Subagent 4, **crypto** (owns `runtime/src/crypto/**`, `std/crypto/**`,
`compiler/src/sema/stdlib_src/crypto*.zl`):
- AES-GCM 0.20x and ChaCha20-Poly1305 0.13x: check CPUID dispatch
  (AES-NI + PCLMUL GHASH, SHA-NI, AVX2 ChaCha/Poly) is present and
  actually taken; `AEAD` objects with an expanded key reused across calls
  and an output buffer the caller provides (today every call re-expands the
  key and allocates);
- Ed25519 sign 0.10x: it derives the public key from the seed on every
  call (two base-point multiplications); precomputed tables for the base
  point; verify 0.28x with a double-scalar multiplication;
- P-256 0.11x: fixed-base and variable-base tables, Montgomery/Solinas
  field arithmetic with 64-bit limbs;
- SHA-256/512 0.59x/0.65x (SHA-NI / unrolled), RSA verify 0.41x, TLS 1.3
  handshakes 0.34x (mostly the above);
- a streaming hash API (`hash.New().Write().Sum()`), missing today;
- keep the test vectors green (`runtime/tests`, `tests/spec/stdlib`), add
  vectors for every optimised path, `make asan` green, constant-time
  behaviour preserved (no secret-dependent branches or indices);
- targets: AES-GCM and ChaCha ≥ 0.7x Go, Ed25519/P-256 ≥ 0.5x, SHA ≥ 0.8x.

Subagent 5, **compiler** (owns `compiler/src/hir/**`, `compiler/src/codegen/**`,
`compiler/src/sema/**` except stdlib sources, DESIGN.md "Память"):
- `defer` allocates two objects and does three retain/release pairs per
  call (12.7x Go): a fixed slot in the frame for defers not in loops, an
  arena/stack for the rest, no heap objects;
- calls through an interface or a closure retain/release the receiver on
  every call (3.9x / 3.0x) even when a slice or the loop variable keeps it
  alive: extend the borrowed-locals analysis (`hir/borrow.rs`) to receivers
  and to `for x in xs { x.M() }`, and to heap-writing functions where the
  root is provably not reassigned during the call;
- a constant error copies its trace on every `return ErrOdd` (5.7x with
  traces): keep the sentinel immutable and attach the trace to the
  propagated value only;
- release builds target the build machine's CPU (`-march=native`): default
  to `x86-64-v2` / `armv8-a`, add `--cpu=native` and `ZELL_TARGET_CPU`,
  document in docs/PLATFORMS.md;
- an ambiguous-name error cascades into bogus follow-on errors
  (RESULTS-framework item 9): report once;
- targets: defer ≤ 2x Go, iface/closure call ≤ 1.5x, sentinel error ≤ 1.5x;
  spec tests for every rule change; no regression on the core table.

Rules for every subagent: spec first (DESIGN.md + grammar + spec tests for
any language-visible change), ABI changes documented on both sides,
`make -C runtime test` + the affected `cargo test` targets green before
reporting, commits on the worktree branch in English imperative, no pushes.
Each report: before/after table for its benchmarks with Go = 1.00, what
changed, what remains.
