# Stdlib benchmarks: Zell vs Go

Five benchmarks of the standard library rather than the language core:
`json_codec`, `encodings`, `crypto`, `file_io`, `sort_collect`. As in the
other benchmarks, each directory holds an equivalent `main.zl` and
`main.go`. Both generate the same deterministic data, so both print the
same checksums to **stdout**, and each prints the time of every phase (and
MB/s or ops/s) to **stderr**.

**Status: no timings yet.** The programs build and give identical stdout
at `SCALE=small` (checked 2026-10-22). Nothing was timed, because five
agents were sharing the machine. The results table below is a TODO for a
run on a quiet machine.

## The benchmarks

| Benchmark | Zell API | Go API | Full size (default) | `SCALE=small` |
|---|---|---|---|---|
| `json_codec` | `encoding.json.Parse<Doc>` / `Format` (typed, `@json(name:)`), `ParseValue` + `Get`/`Index` path lookups + `Value.Format`, and 1M small messages, each parsed with `Parse<Msg>` and formatted again with `Format` | `encoding/json` `Unmarshal` / `Marshal` into structs and into `any` (`map[string]any`) | 100 MB document (~1M items); 1M messages of ~172 bytes (1024 different texts used in turn) | 1 MB, 10k messages |
| `encodings` | `encoding.xml`, `encoding.yaml`, `encoding.toml` `Parse<Catalog>` / `Format`, one struct with `@serde(name:)` + `@xml(attr:, name:)` | `encoding/xml`, `gopkg.in/yaml.v3` v3.0.1, `github.com/BurntSushi/toml` v1.4.0 (vendored) | 20 MB document per format (~150k-185k items), parsed, formatted, re-parsed (round trip checked) | 200 KB each |
| `crypto` | `crypto.hash.Sum`, `aes.GcmSeal`, `chacha.Seal`, `x25519.SharedSecret`, `ed25519.Sign/Verify`, `rsa.SignPKCS1v15/VerifyPKCS1v15`, `ecdsa.Sign/Verify`, `kdf.Hkdf`, `kdf.Argon2id`, `net.tls` Listen/Dial | `crypto/sha256`, `sha512`, `aes`+`cipher.NewGCM`, `x/crypto/chacha20poly1305`, `crypto/ecdh`, `ed25519`, `rsa`, `ecdsa`, `crypto/hkdf` (Go 1.24), `x/crypto/argon2`, `crypto/tls` (x/crypto v0.31.0 vendored) | SHA-256/512 over 1 GB (1 MB chunks); AES-256-GCM and ChaCha20-Poly1305 sealing 1 GB (1 MB messages); 20k X25519; 20k Ed25519 sign + 20k verify; RSA-2048 500 sign + 5000 verify; P-256 10k sign + 10k verify; 200k HKDF-SHA256; 8 Argon2id (t=1, 64 MiB, 4 lanes); 1000 TLS 1.3 full handshakes on loopback | 16 MB, 1/20 of the operations, 1 Argon2id |
| `file_io` | `os.Create/Open`, `File.Write/Read` (1 MB chunks into one reused buffer), `io.NewBufferedWriter/Reader` (1 KB calls), `io.Copy`, `io.NewScanner` + `Bytes()`, `fs.MkdirAll`, `io.WriteFile`, recursive `os.ReadDir` + `DirEntry.Info`, `fs.RemoveAll` | `os`, `bufio.Writer/Reader/Scanner`, `io.Copy`, `os.WriteFile`, `filepath.WalkDir` + `d.Info()`, `os.RemoveAll` | 1 GB file written and read both ways, copied; 1 GB text (~20M lines) scanned; 100k files of 0-199 bytes in 100 x 10 directories created, walked with stat, removed. Peak disk use 2 GB in `$TMPDIR`/`/tmp` | 16 MB, 2 000 files |
| `sort_collect` | `slices.Sort` (ints, strings), `slices.BinarySearch`, `collections.Deque`, `Heap<int>`, `PriorityQueue<Job>`, `LinkedList`, `OrderedMap`, `strings.Builder`, `text.template.Parse<Page>` + `Execute` | `slices.Sort`, `slices.BinarySearch`, a ring-buffer deque (Go has none), `container/heap`, `container/list`, a sorted-array ordered map (Go has none; the same structure as Zell's), `strings.Builder`, `text/template` | 10M ints, 2M strings, 10M lookups, 10M deque pushes, 1M heap and 1M PQ push+pop, 2M list pushes, 50k ordered-map inserts + 1M gets, 100 MB from ~10-byte pieces, 1M template rows | every size / 100 |

Checksums: sums of decoded fields (ids, quantities, prices in cents, counts,
string bytes), a re-parse of the formatted output compared with the first
parse, SHA-256 of concatenated digests, tags and signatures, XOR of shared
secrets and derived keys, sampled bytes of files, and a hash over every
997th sorted element or every 1009th output byte. Randomized results (ECDSA
signatures, TLS sessions) are checked by counting successful verifies and
echoes.

## How to run

```sh
make -C runtime && cargo build --release -p zell
# smoke run of the reduced sizes
SCALE=small ./scripts/bench-perf.sh json_codec encodings crypto file_io sort_collect
# full size, best of 5 alternating (wall, CPU, RSS of the whole program)
RUNS=5 ./scripts/bench-perf.sh json_codec encodings crypto file_io sort_collect
```

`bench-perf.sh` times the whole process and prints only the first 30
characters of the output. For the per-phase numbers, build the two binaries
and run them alternately, keeping stderr:

```sh
b=crypto; out=$(mktemp -d)
./target/release/zell build benchmarks/$b/main.zl --release -o $out/zl
(cd benchmarks/$b && go build -o $out/go main.go)
for i in 1 2 3 4 5; do for l in zl go; do echo "== $l $i"; $out/$l 2>&1 >/dev/null; done; done
```

`encodings` can run a single format with `FORMAT=xml|yaml|toml`. `crypto`
and `encodings` have a `go.mod` with their dependencies in `vendor/`, so
`go build` works offline. The other three use only Go's standard library. `file_io` writes
to `fs.TempDir()` / `os.TempDir()` (`$TMPDIR`, else `/tmp`) and removes its
directory at the end. It needs 2 GB free.

What is comparable, and what is not:

- TLS: Go's client is limited to X25519 (`CurvePreferences`). Go 1.24 would
  otherwise offer the hybrid X25519MLKEM768, which Zell does not implement.
  Both servers present the P-256 certificate of `examples/tls_echo.zl`. Both
  sides do full handshakes: Go's server has session tickets disabled, and
  Zell's client has no session cache.
- AEAD: Go builds the `cipher.AEAD` once and seals into a reused `dst`.
  Zell's API takes the key on every call and returns a new slice (see open
  items).
- `io.Copy`: Go's copy between two `*os.File` uses `copy_file_range`, so
  the kernel copies the data. Zell's goes through a 32 KB user-space buffer.
- `OrderedMap` and the deque: Go has neither in its standard library, so
  `main.go` has the same data structures written out (a sorted array, a ring
  buffer). This compares the implementations, not two libraries.

## Results (TODO: quiet machine, best of 5 alternating, Go = 1.0)

Ratio = Zell time / Go time (for throughput rows: Go rate / Zell rate);
below 1.0 means Zell is faster.

| Benchmark | Phase | Zell | Go | Zell / Go |
|---|---|---|---|---|
| json_codec | typed parse 100 MB | TODO | TODO | TODO |
| json_codec | typed format | TODO | TODO | TODO |
| json_codec | Value parse + lookups | TODO | TODO | TODO |
| json_codec | 1M small messages | TODO | TODO | TODO |
| encodings | XML parse / format 20 MB | TODO | TODO | TODO |
| encodings | YAML parse / format 20 MB | TODO | TODO | TODO |
| encodings | TOML parse / format 20 MB | TODO | TODO | TODO |
| crypto | SHA-256 / SHA-512 MB/s | TODO | TODO | TODO |
| crypto | AES-256-GCM / ChaCha20-Poly1305 MB/s | TODO | TODO | TODO |
| crypto | X25519 ops/s | TODO | TODO | TODO |
| crypto | Ed25519 sign / verify ops/s | TODO | TODO | TODO |
| crypto | RSA-2048 sign / verify ops/s | TODO | TODO | TODO |
| crypto | P-256 sign / verify ops/s | TODO | TODO | TODO |
| crypto | HKDF ops/s, Argon2id ops/s | TODO | TODO | TODO |
| crypto | TLS 1.3 handshakes/s | TODO | TODO | TODO |
| file_io | write / read 1 MB chunks | TODO | TODO | TODO |
| file_io | buffered write / read 1 KB | TODO | TODO | TODO |
| file_io | io.Copy 1 GB | TODO | TODO | TODO |
| file_io | line scan 1 GB | TODO | TODO | TODO |
| file_io | create / walk+stat / remove 100k files | TODO | TODO | TODO |
| sort_collect | sort 10M ints / 2M strings | TODO | TODO | TODO |
| sort_collect | binary search 10M | TODO | TODO | TODO |
| sort_collect | deque / heap / PQ / list / ordered map | TODO | TODO | TODO |
| sort_collect | strings.Builder 100 MB | TODO | TODO | TODO |
| sort_collect | text.template 1M rows | TODO | TODO | TODO |

## Open items found while writing the benchmarks

These come from reading the code and from the small smoke runs, not from
timings.

1. **JSON tokenizer is quadratic. This blocks the full-size `json_codec`
   run.** `scan_string` in `runtime/src/std_json.c` (line 92) allocates
   `zell_alloc(n - *pos + 1)` for every string token: a buffer the size of
   the rest of the document. `zell_alloc` is `calloc`, so every string
   zeroes that buffer. Callgrind on a 0.5 MB array: 99.4% of the
   instructions are `memset` under `calloc` under
   `zell_encoding_json_scan`. Measured with `json.scan`: 4x the input takes
   about 16x the time (20k tokens 1 ms, 80k 17 ms, 320k 283 ms). The 1 MB
   smoke document takes about 1.2 s to parse. At this rate one 100 MB parse
   would take hours, and `json_codec` parses 100 MB three times. All of
   `json.Parse<T>`, `ParseValue` and the small-message loop go through it
   (small messages suffer less: each buffer is at most 200 bytes). Fix: find
   the closing quote first and size the buffer from the string, or unescape
   straight into a `zell__string_alloc` of an upper bound taken from that
   span. Either way, do not zero the buffer.
2. **`json.Format` writes `null` for an empty slice that came from `[]`.**
   `decodeSlice` builds `var res []E = []`, which compares equal to `nil`.
   `write` then emits `null`, so `{"tags":[]}` comes back as
   `{"tags":null}`. Go keeps `[]` (it tells a nil slice from an empty one).
   Because of this, `json_codec` does not print the size of the typed
   output: it differs from Go's by 2 bytes per item without tags. Either
   `Format` should write `[]` for a non-nil empty slice (if the language
   tells them apart), or `DESIGN.md` should say that empty and nil slices
   both format as `null`.
3. **`xml.Format` ignores `@serde(name:)` on attribute fields.**
   `encodeElem` (`std/encoding/xml/xml.zl`, around line 806) names an
   `@xml(attr: true)` attribute from `tag.name`, else the field name.
   `decodeElem` also uses `@serde(name:)`. So
   `@serde(name: "id") @xml(attr: true) Id int` parses `id="8"` but formats
   `Id="8"`, and that output parses back with `Id = 0`. The encoder's
   attribute loop also does not skip `@serde(skip: true)` fields. The
   benchmark works around it with `@xml(attr: true, name: "id")`.
4. **No public streaming hash.** `crypto.hash` exports only one-shot
   functions: `Sha256(string)`, `Sha512(string)` and `Sum(alg, []byte)`.
   The runtime has an incremental hasher (`rtNew`/`rtWrite`/`rtSum`,
   `zell_crypto_hash_rt_*`), but no Zell type wraps it, and no
   `hash.Hash`-style `New("sha256")` with `Write`/`Sum` exists. A 1 GB
   stream cannot get one digest without holding all of it in memory. The
   benchmark hashes each 1 MB chunk separately, and Go does the same
   (`Reset` per chunk) to match.
5. **The AEAD API cannot reuse a key schedule or an output buffer.**
   `aes.GcmSeal(key, nonce, pt, aad)` and `chacha.Seal` take the raw key
   on every call: the AES key schedule and GHASH setup run per message, and
   every call allocates a new output slice. Go builds the AEAD once and
   seals into `dst[:0]`. For 1 MB messages this barely matters; for small
   records (TLS-like sizes) it does. Suggestion: an `aes.NewGcm(key)` /
   `chacha.New(key)` object with `Seal(dst, nonce, pt, aad)`.
6. **Ed25519 signing does two base-point multiplications per signature.**
   `zell__ed25519_sign` (`runtime/src/crypto/ed25519.c:282`) takes only the
   seed. On every call it re-derives the public key (`ge_scalarmult` of the
   base point), then computes R with a second generic `ge_scalarmult` of
   the base point, and runs `consts_init` again. Go keeps the public key in
   its 64-byte `PrivateKey` and uses a precomputed fixed-base table. The
   smoke run already shows signing about 10x slower than Go (to be
   confirmed on a quiet machine). Suggestions: an expanded-key type, or a
   `Sign(priv64, msg)` that takes the public key with it, and a fixed-base
   comb table.
7. **No `fs.WalkDir`/`filepath.WalkDir`.** The tree walk is a hand-written
   recursion over `os.ReadDir` with `DirEntry.Info()` (one `lstat` per
   entry, as in Go's `d.Info()`). It works. Walking a directory tree is
   common enough to deserve an API in `fs` (with `SkipDir`).
8. **`io.Copy` has no fast path.** It always copies through a 32 KB
   user-space buffer (`std/io/io.zl:179`). There is no
   `ReaderFrom`/`WriterTo` protocol, and no `copy_file_range` or `sendfile`
   for file-to-file and file-to-socket copies, which Go uses on Linux.
9. **TLS config has no curve or group preferences.** `tls.Config` has no
   `CurvePreferences`, and the client always offers X25519, P-256 and
   P-384. There is no post-quantum hybrid (X25519MLKEM768, Go 1.24's
   default), so a default Go client and a Zell server negotiate classical
   X25519 only.
10. **No `make([]T, n)`.** Buffers are built by appending in a loop
    (`slices.Of(byte(0), n)`, `io.makeBuf`), which costs one append per
    element. That is an awkward idiom for I/O buffers. A zeroed-allocation builtin or `slices.Make<T>(n)` would
    help.
11. **`strings.Builder` keeps every piece.** It stores a `[]string` of
    parts and joins them in `String()`. Building 100 MB from 10-byte
    pieces keeps about 30M parts alive until the join (10M of them are
    allocated `itoa` strings), while Go
    appends into one growing byte buffer. The memory peak should show up
    as RSS in `bench-perf.sh`.
12. **`collections.OrderedMap` is a sorted array.** `Set` inserts in O(n)
    by shifting element by element (`std/collections/collections2.zl:650`).
    It is not a tree or B-tree, so 1M random inserts would be quadratic.
    The benchmark uses 50k inserts, and Go gets the same structure.
13. **`text.template.Execute` returns a string and converts all the data
    first.** `Execute` turns the whole data value into a dynamic `Value`
    tree (1M rows become 1M `Value` structs with field slices) before it
    renders. It has no `io.Writer` variant, so the output of 1M rows is
    built as one string. Go streams to a writer and reflects lazily.

Smaller notes: `crypto.hash.Sum` picks the algorithm by name on every
call. `ecdsa.Sign` takes a bare 32-byte scalar and returns DER, so a Go
key has to be rebuilt from the scalar for the comparison. `os.ReadDir`
returns entries sorted, as Go's does.
