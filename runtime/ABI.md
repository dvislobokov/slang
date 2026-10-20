# Slang runtime ABI, v0.1

This document is the contract between the Slang compiler (Rust + LLVM, which emits
LLVM IR) and the C runtime library `libslang_rt.a`. Everything listed here is
stable for 0.1. Anything not listed (files under `src/`, symbols starting with
`slang__`, opaque struct contents) is private and may change.

The public header is `runtime/include/slang_rt.h`. If this document and the
header disagree, that is a bug; report it.

Contents

1. [Building and linking](#1-building-and-linking)
2. [General conventions](#2-general-conventions)
3. [Memory and reference counting](#3-memory-and-reference-counting)
4. [Strings](#4-strings)
5. [Slices](#5-slices)
6. [Maps](#6-maps)
7. [Errors](#7-errors)
8. [Panics](#8-panics)
9. [I/O](#9-io)
10. [Tasks, async/await and the scheduler](#10-tasks-asyncawait-and-the-scheduler)
11. [Program entry](#11-program-entry)
12. [Environment variables](#12-environment-variables)
13. [Known limits in v0.1](#13-known-limits-in-v01)
14. [Standard library](#14-standard-library)
15. [Platform layer](#15-platform-layer)
16. [Networking: std.net and std.net.http](#16-networking-stdnet-and-stdnethttp)
17. [Reflection tables and std.encoding.json](#17-reflection-tables-and-stdencodingjson)

---

## 1. Building and linking

```
make            # -> runtime/libslang_rt.a (also runtime/build/libslang_rt.a)
make test       # C unit tests + a real llvm.coro example (tests/coro_lowering.ll)
make clean
make asan       # clean rebuild + all tests under AddressSanitizer/LeakSanitizer
```

`CFLAGS_EXTRA=...` is appended to the compile flags (and to the link of the
tests), e.g. `make clean && make CFLAGS_EXTRA=-fsanitize=address test`. Prefer
`make asan`: it also installs `tests/lsan.supp` (immortal literals and sentinels
are never freed by design) and works around a fork-under-ASan deadlock in
`test_task`.

Link a Slang program with:

```
clang prog.o runtime/libslang_rt.a -lpthread -lm -rdynamic -o prog
```

`-rdynamic` is optional. It only makes function names appear in panic backtraces.
`-lm` is required since the `std.math` functions (section 14) call libm.

Target: x86-64 / AArch64 Linux, LP64, glibc (the panic backtrace uses
`<execinfo.h>`). The library is built with `-fPIC`, so you can also link it into
shared objects.

---

## 2. General conventions

### 2.1 Type mapping

| C type in header | LLVM IR type | Notes |
|---|---|---|
| `int64_t`, `uint64_t` | `i64` | Slang `int`, `uint`, lengths and indices are all `i64` |
| `int32_t` | `i32` | Slang `char` (rune) is `i32` |
| `uint8_t` | `i8` | Slang `byte` |
| `double` | `double` | Slang `float` |
| `bool` | `zeroext i1` | C `_Bool`. Declare both parameters **and** return values `zeroext i1`. In memory it is 1 byte |
| `T *`, any object pointer | `ptr` | |
| function pointers | `ptr` | always C calling convention (`ccc`) |

All runtime functions use the platform C calling convention. **No runtime function
takes or returns a struct by value.** Aggregates (`slang_slice`, `slang_map_iter`)
are always passed as `ptr`, and functions that produce one write it through an
out-pointer (`slang_slice *out`). That way the compiler never has to reproduce the
SysV/AAPCS rules for passing structs.

### 2.2 Ownership (the most important rule)

The runtime uses automatic reference counting. Every heap object carries an atomic
refcount (section 3).

1. **Arguments are borrowed.** A runtime function never consumes a reference the
   caller passed in. If it keeps the value (it stores it in a slice, map, error,
   task result or channel), it calls retain itself.
2. **Returned heap objects are owned (+1).** The caller must eventually call
   `slang_release` on them. The exceptions are marked **[borrowed]** in this
   document. Those return a pointer that stays valid only while the object it was
   read from stays alive and unmodified.
3. **Out-pointer aggregates are owned.** A `slang_slice` written to `*out` holds a
   reference to its backing array. The caller releases it with
   `slang_slice_release`.
4. **In-place mutators** (`slang_slice_append*`, and `slang_slice_sub` with
   `out == s`) update a slice variable that the caller owns. The old reference
   held by that variable is transferred or released internally, so the caller has
   nothing to fix up.
5. **Pointers into containers are borrowed.** This covers `slang_slice_index`,
   `slang_map_get*`, `slang_task_result` and `slang_task_await`. Writing through
   them is allowed, and it is an ordinary store into a slot: see 2.3.

### 2.3 What the compiler must do for its own variables and fields

For any location (local, struct field, slice element, map value slot) whose type
is a reference type (string, `*T` to a heap object, map, error, Task, closure,
Mutex, Channel):

- **Store** `loc = v`: `slang_retain(v)` first, then `slang_release(old)`, then
  write. Retaining first makes `x = x` safe.
- **Copy into a new variable or argument passed by value**: retain.
- **End of scope / overwrite / drop of the containing object**: release.
- For a slice-typed location, use `slang_slice_retain` / `slang_slice_release` /
  `slang_slice_assign` instead. A slice value owns exactly one reference to
  `backing`.
- For struct values that contain references, emit per-type "retain fields" and
  "release fields" helpers and use them as `slang_value_ops` (section 3.3).

Returning a value from a function transfers +1 to the caller. This is the same
rule as the runtime's own functions.

### 2.4 NULL

NULL is a valid value wherever the header takes a heap object, and it means:

| Type | NULL means |
|---|---|
| `slang_string *` | the empty string `""`. Every string function accepts NULL. The zero value of `string` is NULL |
| `slang_error *` | no error (`nil`) |
| `slang_map *` | nil map: `len` is 0, `get` returns NULL, `delete` is a no-op, iteration is empty, **`set` panics** |
| `slang_slice` | the all-zero struct is the nil or empty slice |
| `slang_task *`, `slang_mutex *`, `slang_chan *` | invalid: `slang_panic_nil()` |
| `slang_retain(NULL)`, `slang_release(NULL)` | no-op |

The compiler checks dereferences of user pointers itself:
`if (p == null) call slang_panic_nil()`.

### 2.5 Thread safety

- Refcount operations are atomic, so objects can be shared between tasks or
  threads.
- Strings and errors are immutable, so they are freely shareable.
- Slices and maps are **not** internally synchronised, the same as in Go. Concurrent
  mutation needs a `Mutex` or a channel.
- Tasks, mutexes, channels and the scheduler are fully thread-safe.
- `println_*` writes one whole line under the stdio lock, so lines from
  different threads never interleave.

---

## 3. Memory and reference counting

### 3.1 Layouts

```c
typedef struct slang_type_info {      // 24 bytes, usually a constant global
    const char *name;                 // +0   type name (may be NULL)
    int64_t     size;                 // +8   informational object size, 0 if unknown
    void      (*drop)(void *obj);     // +16  may be NULL
} slang_type_info;

typedef struct slang_object {         // 16 bytes: header of EVERY heap object
    int64_t                refcount;  // +0   atomic; -1 = immortal
    const slang_type_info *type;      // +8   may be NULL
} slang_object;                       // payload starts at +16
```

```llvm
%slang_type_info = type { ptr, i64, ptr }
%slang_object    = type { i64, ptr }
```

Runtime functions always take and return pointers to the **header** (offset 0)
of a heap object. Compiled code represents a Slang `*T` as a pointer to the
**payload** (header + 16): for `new Point{X: 1, Y: 2}` the object is
`{ %slang_object, double, double }` and the `*Point` points at field `X`.
Use `slang_ptr_retain` / `slang_ptr_release` / `slang_ops_ptr` for such
values; a non-nil `*T` always points at the payload of a whole object (the
compiler never retains interior pointers, see DESIGN.md "Память").

The refcount word holds the count in its low 62 bits
(`SLANG_RC_COUNT_MASK`); bit 62 (`SLANG_RC_WEAK_FLAG`) marks an object that
has a weak box (3.4). `-1` (`SLANG_RC_IMMORTAL`) is immortal.

### 3.2 Functions

| Function | Semantics |
|---|---|
| `void *slang_alloc(int64_t size)` | `calloc`-style **zeroed** raw memory, not refcounted. Use it for coroutine frames, closure environments without a header, and scratch memory. Out of memory or a negative size panics. `size == 0` returns a unique pointer |
| `void *slang_realloc(void *p, int64_t size)` | raw `realloc`. New bytes are **not** zeroed. Panics on out of memory |
| `void slang_free(void *p)` | frees raw memory. NULL is fine |
| `void *slang_object_new(const slang_type_info *type, int64_t size)` | allocates a zeroed object of `size` bytes, **header included** (so `size >= 16`), sets `refcount = 1` and `type = type`. Returns it +1 |
| `void slang_retain(void *obj)` | atomic `++refcount`. No-op for NULL and for immortal objects |
| `void slang_release(void *obj)` | atomic `--refcount`. When it reaches 0: calls `type->drop(obj)` if both are non-NULL, then frees the memory. No-op for NULL and immortal objects |
| `int64_t slang_refcount(const void *obj)` | current count (debugging and tests), without the weak flag. 0 for NULL, -1 for immortal objects |
| `int64_t slang_live_objects(void)` | number of objects allocated with `slang_object_new` and not yet freed (leak checks in tests). Objects made immortal (`slang_string_literal`, `slang_error_sentinel`) do not count |
| `void slang_ptr_retain(void *payload)` / `void slang_ptr_release(void *payload)` | retain / release the object whose payload (object + 16) is given: the operations on a `*T`. No-op for NULL |
| `void slang_leak_check_enable(int32_t mode)` | leak check (3.5); call it before anything else, even before `slang_runtime_init` |

**The `drop` contract.** `drop(obj)` must release every reference the object holds:
strings, pointers, slices (`slang_slice_release`) and so on. It must **not** free
`obj`, because the runtime does that after `drop` returns. The compiler emits one
`drop` per heap-allocated struct type that has reference-typed fields. If there
are no such fields, `type` may point to a type info whose `drop` is NULL, or
`type` itself may be NULL.

**Allocation (implementation note, not ABI).** Objects of at most 128 bytes
(header included) come from a small-object allocator: per-thread free lists
per 16-byte size class, carved from 4 MB regions that are never returned to
the OS; a bitmap over the address space identifies region memory on free.
Larger objects use `malloc`. It is disabled under AddressSanitizer, with leak
check mode 2 and with `SLANG_NO_SLAB=1`. Nothing outside the runtime may free
or `realloc` object memory itself. `slang_release` skips the atomic
decrement when it reads a count of exactly 1 with no weak flag (the caller
holds the only reference, so no other thread can retain it); otherwise the
decrement is `acq_rel`, and `slang_retain` is a relaxed increment.

**Immortal objects** (`refcount == -1`, `SLANG_RC_IMMORTAL`) are never modified or
freed. Use them for string literals and sentinel errors emitted as LLVM globals.

When the count of an object with a weak box reaches zero, the weak box is
cleared **before** `drop` runs, so weak references read as NULL from then on
(including inside the destructor). There is no cycle collector: use weak
references for back pointers.

### 3.3 Value ops (ownership of inline values in containers)

Containers (slices, maps, task results, channels) store values inline as raw bytes.
To know how to retain or release those bytes they take an optional
`slang_value_ops *`:

```c
typedef struct slang_value_ops {   // 16 bytes
    void (*retain)(void *slot);    // +0  add the references contained in *slot
    void (*release)(void *slot);   // +8  drop the references contained in *slot
} slang_value_ops;
```

- `NULL` ops means plain data (ints, floats, bools, structs with no references).
- `&slang_ops_ref`: the slot is one 8-byte heap-object pointer (string, map,
  error, Task, weak box...).
- `&slang_ops_ptr`: the slot is one `*T` (a payload pointer, object + 16).
- `&slang_ops_slice`: the slot is one 32-byte `slang_slice`.
- For struct values containing references, the compiler emits a constant
  `slang_value_ops` whose functions retain or release each field.

`release` must handle an all-zero slot (all release functions treat NULL as a
no-op), because containers zero-initialise their storage.

### 3.4 Weak references

A `weak *T` is a pointer to a **weak box**, a small refcounted object
`{hdr, slang_object *target}` shared by all weak references to the same
object, or NULL. Boxes are kept in a side table (object -> box) protected by a
mutex; an object with a box has `SLANG_RC_WEAK_FLAG` set in its refcount word.

| Function | Semantics |
|---|---|
| `void *slang_weak_new(void *payload)` | the weak box of the object whose payload is given, created on first use; returns it **+1** (the box, not the object, is retained). NULL gives NULL. Panics for immortal objects |
| `void *slang_weak_get(void *box)` | the target's payload **+1** (retained), or NULL if the target was freed (or `box` is NULL). Never resurrects an object whose count already reached zero |

Weak boxes are ordinary objects: retain and release them with
`slang_retain`/`slang_release` (`slang_ops_ref` in containers). When the
target dies the box's `target` becomes NULL; when the last weak reference
goes away the box removes itself from the table and clears the flag.

### 3.5 Leak check

`slang_leak_check_enable(mode)`: mode 1 counts live objects; mode 2 also
links every object into a list (a 16-byte prefix in front of the header, so
it must be enabled before the first allocation). At the end of
`slang_runtime_shutdown`, if `slang_live_objects() > 0`, the runtime prints

```
leak: N objects
  K x TypeName        (mode 2 only, by count, then name)
```

to stderr and exits with status `SLANG_LEAK_EXIT_CODE` (66). The compiler
enables it for `--leak-check` (mode 2 in debug builds, 1 with `--release`)
and releases the program's globals before calling the shutdown.

---

## 4. Strings

### 4.1 Representation

A string is a **single pointer** to an immutable heap object. **NULL is `""`.**

```c
typedef struct slang_string {
    slang_object hdr;   // +0   type = &slang_type_string
    int64_t      len;   // +16  length in BYTES, excluding the terminator
    char         data[];// +24  len bytes of UTF-8, then one NUL byte
} slang_string;         // sizeof == 24 + len + 1
```

The bytes are UTF-8 but not validated. `data[len]` is always `'\0'`, so `data` can
be passed to C, although an embedded NUL is possible. A string's length and bytes
never change after creation.

**Literals.** The compiler emits each literal as an immortal constant global and
uses its address directly. There is no runtime call and nothing to release:

```llvm
@.str.hello = private unnamed_addr constant { i64, ptr, i64, [6 x i8] }
    { i64 -1, ptr @slang_type_string, i64 5, [6 x i8] c"hello\00" }, align 8
; use: ptr @.str.hello
```

(retain and release on it are no-ops). `slang_string_literal(bytes, len)` creates
the same kind of immortal string at run time. Call it at most once per literal, for
example in a module initialiser, because each call leaks one object by design. In
C, `SLANG_STATIC_STRING(name, "lit")` declares a static literal.

### 4.2 Functions

All `slang_string *` returns are **owned (+1)**. All string arguments are borrowed
and may be NULL.

| Function | Semantics |
|---|---|
| `slang_string_new(const char *bytes, int64_t len)` | copy of `len` bytes |
| `slang_string_from_cstr(const char *s)` | copy of a NUL-terminated C string (NULL gives `""`) |
| `slang_string_literal(const char *bytes, int64_t len)` | immortal copy (see above) |
| `const char *slang_string_data(s)` | **[borrowed]** pointer to the bytes, NUL-terminated. `""` for NULL |
| `int64_t slang_string_len(s)` | length in bytes (`len(s)`) |
| `int64_t slang_string_rune_count(s)` | number of code points. Each invalid byte counts as 1 |
| `slang_string_concat(a, b)` | `a + b`. Always returns a new object |
| `slang_string_append(a, b)` | `a + b`, **consuming** the caller's reference to `a`. If `a` is uniquely referenced it is extended in place (with spare capacity, so `s += x` in a loop is linear); otherwise like `concat` followed by `release(a)`. `b` may be `a` |
| `int32_t slang_string_compare(a, b)` | bytewise comparison, returns -1, 0 or 1 (for `<`, `<=` and so on) |
| `bool slang_string_eq(a, b)` | byte equality (`==`). `NULL == ""` |
| `uint64_t slang_string_hash(s)` | 64-bit FNV-1a of the bytes. Stable within a process |
| `slang_string_slice(s, lo, hi)` | `s[lo:hi]` by **bytes**, as a copy. Panics unless `0 <= lo <= hi <= len` |
| `uint8_t slang_string_byte_at(s, i)` | `s[i]`. Panics on out-of-bounds |
| `int32_t slang_string_decode_rune(s, i, int64_t *width)` | decodes the UTF-8 rune starting at byte `i` and stores its byte width (1..4) in `*width` (which may be NULL). An invalid sequence gives `U+FFFD` with width 1. Panics if `i` is out of range. Use it to lower `for i, c in str`: advance `i += width` |
| `int64_t slang_string_index(s, sub)` | byte offset of the first occurrence, or -1. An empty `sub` gives 0 |
| `bool slang_string_contains / has_prefix / has_suffix(s, x)` | as the names say |
| `slang_string_from_int(int64_t)` / `from_uint(uint64_t)` | decimal |
| `slang_string_from_float(double)` | shortest decimal that round-trips, `%g` style: `0.5`, `3`, `0.1`, `1e+21`, `NaN`, `+Inf`, `-Inf`. Note that `3.0` prints as `3`, as in Go |
| `slang_string_from_bool(bool)` | `"true"` or `"false"` |
| `slang_string_from_rune(int32_t)` | UTF-8 encoding. An invalid code point gives U+FFFD |
| `int64_t slang_string_to_int(s, slang_error **err)` | base-10 with an optional sign (`+` or `-`). No surrounding whitespace (any `isspace` byte) is allowed. On success `*err = NULL`. On failure it returns 0 and sets `*err` to an owned error (`invalid integer: "..."` or `integer out of range: "..."`). `err` may be NULL |
| `double slang_string_to_float(s, slang_error **err)` | `strtod` syntax (it also accepts `inf`, `nan` and hex floats). The error convention is the same |

Conversions between strings and `[]byte` are in section 5.

---

## 5. Slices

### 5.1 Representation

```c
typedef struct slang_slice {   // 32 bytes, 8-aligned
    void         *data;        // +0   pointer to element 0 of this view
    int64_t       len;         // +8
    int64_t       cap;         // +16  elements available from data
    slang_object *backing;     // +24  OWNED reference to the backing array, or NULL
} slang_slice;
```

```llvm
%slang_slice = type { ptr, i64, i64, ptr }
```

- `{null, 0, 0, null}` is the nil or empty slice, and it is valid everywhere.
- A slice is a **value type** held in an `alloca` or a struct field. Every live
  slice value owns one reference to `backing`.
- **Copy** with `slang_slice_assign(&dst, &src)`, or memcpy followed by
  `slang_slice_retain`. **Drop** with `slang_slice_release(&s)`, which also zeroes it.
- Sub-slices share the backing array, as in Go, so writes through one are visible
  through the other.
- `elem_size` and `ops` are **not stored in the slice**. The compiler passes them to
  every call (it knows them statically). The backing array keeps its own copies so
  it can release the elements when it dies.

The backing array is an object with this layout (for debugging only, do not use it
directly):
`{ slang_object hdr; int64 elem_size @16; int64 cap @24; const slang_value_ops *ops @32; pad @40; elements @48 }`.

**Element ownership invariant.** The backing array owns one reference for every
non-zero element slot in `[0, cap)`, and it releases all of them when freed.
Writing an element through `slang_slice_index` is therefore a normal store: retain
the new value, release the old one, write.

### 5.2 Functions

`elem_size` is `sizeof(T)` in bytes. Elements are guaranteed 8-byte alignment.

| Function | Semantics |
|---|---|
| `slang_slice_new(out, elem_size, len, cap)` | `make([]T, len, cap)` with POD elements. Zeroed. Panics unless `0 <= len <= cap`. `cap == 0` produces the nil slice. `*out` is overwritten, not released |
| `slang_slice_new_ops(out, elem_size, len, cap, ops)` | the same, with element ownership `ops` |
| `slang_slice_from(out, elem_size, ops, const void *elems, n)` | slice literal `[]T{...}`: copies `n` elements from a C array and **retains** each one (the source stays owned by the caller) |
| `slang_slice_retain(const slang_slice *s)` | +1 on `s->backing` |
| `slang_slice_release(slang_slice *s)` | -1 on `s->backing`, then `*s = {0}` |
| `slang_slice_assign(dst, src)` | `dst = src` (retains src, releases the old dst, copies). `dst == src` is safe |
| `void *slang_slice_index(s, i, elem_size)` | **[borrowed]** address of element `i`. Panics `index out of range [i] with length len` unless `0 <= i < len`. The compiler may instead inline `i <u len` and call `slang_panic_bounds` |
| `slang_slice_append(slang_slice *s, elem_size, ops, const void *elem)` | `s = append(s, elem)` **in place**. The element is copied and retained. If `len < cap` it writes into the shared backing, like Go. Otherwise it allocates a new backing array (growth: at least 4, doubling below 1024 elements, then x1.5), copies and retains the elements, and releases the old backing; if `s` is the only reference to its backing (and starts at its first element) the backing is instead resized in place and the elements keep their references. `elem` may point into `s` itself. For `t = append(s, x)` with `t != s`, first do `slang_slice_assign(&t, &s)`, then append to `t`. The compiler inlines the `len < cap` case (store the element by the 2.3 store rule, `len += 1`) and calls this function only when `len == cap` |
| `slang_slice_append_slice(s, elem_size, ops, const slang_slice *other)` | `s = append(s, other...)` in place. `other` may alias `s` (memmove semantics) |
| `slang_slice_sub(out, s, lo, hi, elem_size)` | `s[lo:hi]`, with `cap = s.cap - lo`. Panics unless `0 <= lo <= hi <= cap`. Returns a +1 reference in `*out`. If `out == s` (`s = s[lo:hi]`), the old reference is released. Otherwise `*out` is overwritten without being released, so release it first if it held a slice |
| `int64_t slang_slice_copy(dst, src, elem_size, ops)` | Go `copy(dst, src)`. Copies `min(len)` elements with memmove semantics (overlap is fine), retains the copied values and releases the overwritten ones. Returns the count |
| `slang_string *slang_string_from_bytes(const slang_slice *b)` | `string(b)`, a copy (`elem_size` 1) |
| `slang_string_to_bytes(slang_slice *out, const slang_string *s)` | `[]byte(s)`, a copy |

---

## 6. Maps

### 6.1 Representation

`map[K]V` is a pointer to an opaque refcounted object (`slang_map *`). Assigning a
map variable copies the pointer (retain/release), giving reference semantics as in
Go. NULL is the nil map (see 2.4).

Keys and values are stored inline as `key_size` and `val_size` bytes. The map
**retains** the keys and values it stores (using `key_ops` and `val_ops`) and
releases them when it overwrites them, deletes them or is destroyed.

```c
typedef uint64_t (*slang_hash_fn)(const void *key);          // key points to key_size bytes
typedef bool     (*slang_eq_fn)(const void *a, const void *b);

typedef struct slang_map_desc {       // 48 bytes; copied by slang_map_new
    int64_t key_size;                 // +0   > 0
    int64_t val_size;                 // +8   >= 0 (0 for a set)
    slang_hash_fn hash;               // +16
    slang_eq_fn   eq;                 // +24
    const slang_value_ops *key_ops;   // +32  may be NULL
    const slang_value_ops *val_ops;   // +40  may be NULL
} slang_map_desc;
```

Built-in hash and eq functions: `slang_hash_int` / `slang_eq_int` (the key is an
`int64_t`), `slang_hash_str` / `slang_eq_str` (the key is a `slang_string *` slot,
compared by content), and `slang_hash_bytes(p, n)` for building your own. For
struct keys, the compiler generates `hash` (for example by combining
`slang_hash_bytes` over the fields, or calling `slang_string_hash` for string
fields) and a field-wise `eq`. Do not memcmp a struct that has padding unless the
padding is zeroed.

### 6.2 Functions

`key` and `val` arguments point to `key_size` and `val_size` bytes. They are
borrowed.

| Function | Semantics |
|---|---|
| `slang_map *slang_map_new(const slang_map_desc *d, int64_t hint)` | new empty map, +1. `hint` pre-sizes the table for that many entries |
| `slang_map_new_str(val_size, val_ops)` | `map[string]V`. The key slot is `slang_string *` (with `slang_ops_ref`) |
| `slang_map_new_int(val_size, val_ops)` | `map[int]V`. The key slot is `int64_t` |
| `int64_t slang_map_len(m)` | number of entries (0 for nil) |
| `void *slang_map_get(m, key)` | **[borrowed]** pointer to the value slot, or NULL if absent. The pointer is valid until the next `set`, `get_or_insert` or `delete` on the map. `v, ok = m[k]`: `ok = p != NULL`, and `v` is `*p` (retain it) or the zero value |
| `void slang_map_set(m, key, val)` | `m[k] = v`. Inserts (retaining key and value) or overwrites (retains the new value, releases the old one). Panics `assignment to entry in nil map` if `m` is NULL |
| `void *slang_map_get_or_insert(m, key)` | **[borrowed]** value slot for `key`, inserting a zeroed value if absent. Use it for `m[k] += 1` and `m[k].f = x`. Writing to the slot follows the store rule in 2.3. Panics on nil |
| `bool slang_map_delete(m, key)` | removes the entry (releasing the key and value). Returns whether it existed. A nil map gives false |
| `slang_map_get_str / set_str / delete_str(m, slang_string *key, ...)` | fast paths for string-keyed maps. The key is passed directly, not by pointer |
| `slang_map_get_int / set_int / delete_int(m, int64_t key, ...)` | fast paths for int-keyed maps |
| `void *slang_map_get_or_insert_str(m, slang_string *key)` / `slang_map_get_or_insert_int(m, int64_t key)` | **[borrowed]** `get_or_insert` fast paths (added in v0.1.1; the compiler uses them for `m[k] op= v` and `m[k].f = x` on string/int keys) |

The `_str`/`_int` entry points require a map created for that key type
(`slang_map_new_str`/`_int`, or `slang_map_new` with `slang_hash_str`/`slang_eq_str`
or `slang_hash_int`/`slang_eq_int`, which select the same specialised paths).

**Implementation (not ABI):** open addressing in SwissTable style: one
control byte per slot (empty, deleted or 7 hash bits), groups of 8 matched
with SWAR, triangular probing over groups, power-of-two capacity, growth at
7/8 load. Entries live inline in one table and move only when it is resized,
so a value pointer from `get`/`get_or_insert` stays valid until the next
insertion of a new key or `delete`. String keys are hashed internally with a
word-at-a-time hash (`slang_string_hash` itself stays FNV-1a); int keys with a
64-bit mixer.

### 6.3 Iteration

```c
typedef struct slang_map_iter { slang_map *map; int64_t bucket; void *next; int64_t reserved; } slang_map_iter; // 32 bytes, private
void slang_map_iter_init(slang_map_iter *it, slang_map *m);
bool slang_map_iter_next(slang_map_iter *it, void **key, void **val);  // [borrowed] slots
```

To lower `for k, v in m`: `alloca` an iterator, call `init`, then loop on `next`.
`key` and `val` receive pointers into the map (either may be NULL if not wanted).
Copy and retain them into the loop variables.

- The iterator does **not** retain the map. The compiler must keep the map alive
  for the duration of the loop. It usually already holds it in a variable;
  otherwise retain it before the loop and release it after.
- The order is unspecified (it is table-slot order, not randomised).
- **Deleting the entry just returned is allowed.** Inserting into the map, or
  deleting any other entry, during iteration is undefined behaviour (entries may
  be skipped or visited twice, and the iterator may read freed memory).

---

## 7. Errors

### 7.1 Representation

`error` values are pointers to immutable refcounted `slang_error` objects. NULL
means no error.

```c
typedef slang_string *(*slang_message_fn)(void *payload);  // returns an OWNED string

typedef struct slang_error {            // 224 bytes
    slang_object hdr;                   // +0   type = &slang_type_error
    slang_string *message;              // +16  own message, or the wrap context (owned)
    struct slang_error *inner;          // +24  wrapped error (owned) or NULL
    const slang_type_info *user_type;   // +32  type of a boxed user error, or NULL
    void *payload;                      // +40  boxed user value (a slang object, owned) or NULL
    slang_message_fn message_fn;        // +48  user Message() thunk or NULL
    // error trace (7.4), all zero for an error without one:
    struct slang_error *origin;         // +56  error this is a copy-on-write copy of (owned) or NULL
    const slang_error_site *created;    // +64  creation site or NULL
    int64_t nframes;                    // +72  number of `returned via` frames
    slang_error_frame frames[8];        // +80  first 8 frames {site, ctx}
    slang_error_frame *overflow;        // +208 frames 8.. (raw heap array) or NULL
    int64_t overflow_cap;               // +216
} slang_error;
```

Treat the fields as read-only and use the functions below. The layout is
documented so the compiler can emit **static sentinel errors** directly:

```llvm
; const ErrNotFound = error("not found")
@ErrNotFound = constant { i64, ptr, ptr, ptr, ptr, ptr, ptr, [168 x i8] }
    { i64 -1, ptr @slang_type_error, ptr @.str.not_found, ptr null, ptr null, ptr null, ptr null,
      [168 x i8] zeroinitializer }   ; the trace fields stay zero
```

`slang_error_sentinel(msg)` creates an immortal error at run time instead (for
example in a module initialiser).

### 7.2 Functions

| Function | Semantics |
|---|---|
| `slang_error_new(const slang_string *msg)` | `error(msg)`, +1. Retains `msg` |
| `slang_error_new_cstr(const char *msg)` | the same, from a C string |
| `slang_error_sentinel(msg)` | immortal error (for `const E = error("...")`). Never freed |
| `slang_error_new_typed(const slang_type_info *type, void *payload, slang_message_fn fn)` | boxes a user error (any struct with `Message()`). `payload` must be a slang heap object holding the struct value (for example `slang_object_new(&NotFound_type, 16 + sizeof(NotFound))` with the fields copied in). It is **retained**. `fn(payload)` is the compiler-generated thunk that calls the user's `Message()` and returns an owned string |
| `slang_error_wrap(slang_error *inner, const slang_string *ctx)` | `err?("ctx")` and `errors.Wrap(err, ctx)`: a new error whose own text is `ctx`, wrapping `inner` (retained). **`inner == NULL` returns NULL** |
| `slang_error_unwrap(const slang_error *e)` | `errors.Unwrap`: the inner error, +1, or NULL |
| `slang_string *slang_error_message(const slang_error *e)` | owned full text: own text, then `": "` plus the text of each inner error down the chain, for example `startup: loading config: file missing`. The own text of a typed error is `message_fn(payload)`. NULL gives `"<nil>"` |
| `bool slang_error_is(const slang_error *err, const slang_error *target)` | `errors.Is`: walks `err`, `err->inner`, ... and returns true if any link **is the same object** (pointer identity) as `target`, where a copy-on-write copy (7.4) counts as its `origin`. Sentinels work because they are unique objects. `is(NULL, NULL)` is true and `is(x, NULL)` is false for non-NULL `x` |
| `void *slang_error_as(const slang_error *err, const slang_type_info *type)` | `errors.As`: the **[borrowed]** payload of the first link with `user_type == type`, or NULL. The compiler copies (and retains) the struct out of `payload + 16` into the target |
| `slang_err_eof` | immortal sentinel `"EOF"`, returned by `slang_read_line` |

Note: DESIGN.md says `errors.Is` compares "by value". In 0.1 the runtime compares
identity, which matches sentinel usage. Value comparison of typed errors, if
wanted, can be added by the compiler before calling `slang_error_is`.

### 7.3 Recommended lowering of `T!` (compiler-side, not enforced)

- `T!` is the pair `{ T value; ptr err }`. It is returned through an sret
  out-pointer or as an LLVM first-class aggregate between Slang functions; this
  never crosses the runtime ABI. `err == null` means success.
- `return error("x")` becomes `{zero, slang_error_new(@.str.x)}`, and `return v`
  becomes `{v, null}`.
- `e?` becomes: if `err != null`, release any partially built locals and return
  `{zero, err}`, moving the reference.
- `e?("ctx")` becomes: `w = slang_error_wrap(err, @.str.ctx)`, then
  `slang_release(err)`, then return `w` (with traces on, both steps are
  `w = slang_error_returned(err, @site, @.str.ctx)`, see 7.4).
- Inside a block `try { } catch err { }`, `e?` does not return: it releases
  the temporaries and block locals of the `try` body, stores the error and
  branches to the `catch` block.
- `try e catch err { ... }` binds `err` (owned) and releases it at the end of the
  catch scope. `try e else d` releases `err` and uses `d`.
- Runtime functions that can fail use `slang_error **err` out-parameters
  (`*err = NULL` on success, an owned error on failure), or they return
  `slang_error *` directly (NULL = ok).

### 7.4 Error traces

Every error can carry a *return trace* (DESIGN.md "Трасса ошибки"): its
creation site plus one frame per `?` it passed and per `try ... catch` that
caught it. Sites are static, read-only records emitted by the compiler:

```c
typedef struct slang_error_site {   // 24 bytes
    const char *file;               // +0  source path as given to the compiler
    const char *func;               // +8  "Open", "Cache.Get", "main$lambda1"
    int64_t line;                   // +16 1-based
} slang_error_site;

typedef struct slang_error_frame {  // 16 bytes
    const slang_error_site *site;
    slang_string *ctx;              // the `?("ctx")` context (owned) or NULL
} slang_error_frame;
```

| Function | Semantics |
|---|---|
| `slang_error *slang_error_created(slang_error *err, const slang_error_site *site)` | **consumes** `err`, returns it (or its copy) owned. Sets the creation site unless one is already set. NULL err -> NULL. The compiler calls it after `error(..)` / `errors.New`, after boxing a struct as `error`, on the result of `errors.Wrap`, on errors returned by fallible stdlib externs (the call is the creation site), and on the value of `return err` (so a sentinel gets the site of its `return`) |
| `slang_error *slang_error_returned(slang_error *err, const slang_error_site *site, const slang_string *ctx)` | **consumes** `err`, returns the error to propagate (owned). With `ctx` it first wraps (`slang_error_wrap`) and moves the trace into the wrapper; then it appends the frame `{site, ctx}`. Used by `e?` / `e?("ctx")` (also inside a block `try`) and by `try e catch err` before `err` is bound |
| `slang_string *slang_error_trace(const slang_error *err)` | owned trace text, `""` for NULL or no trace. Format below |
| `void slang_error_frames(const slang_error *err, slang_slice *out)` | owned `[]errors.Frame`: one 32-byte record `{string File @0, int64 Line @8, string Func @16, string Context @24}` per trace line (`Context` is the `?("...")` text or NULL), elements owned via `slang_ops_error_frame` |
| `slang_errors_trace` / `slang_errors_frames` | the `std.errors.Trace` / `Frames` externs (same functions) |
| `void slang_error_report(const slang_error *err)` | writes `error: <message>`, then the trace (if any), to stderr. The C `main` calls it when `func main() void!` (or `async func main() void!`) returned an error, after `slang_runtime_shutdown`, and then exits with code 1 |

- **Storage**: the first 8 frames live inline in the error object; more go to
  a growable raw array (`overflow`, doubling from 8). One `?` costs storing
  two pointers.
- **Copy-on-write**: if `err` is shared (`refcount != 1`, including immortal
  sentinels), both functions first make a shallow copy (message, inner,
  payload retained; `origin` = the original's root, retained; the trace
  copied) and release the original, so other holders never see a changed
  trace. `slang_error_is` treats a copy as its `origin`.
- **Text** (`errors.Trace`): lines joined by `\n`, no trailing newline:
  `  created at   <loc>  <func>` (if there is a creation site), then for each
  frame, oldest first, `  returned via <loc>  <func>[  ?("<ctx>")]`, where
  `<loc>` = `<file>:<line>` is padded to the longest location, `<func>` is
  padded to the longest function name when a context follows, and trailing
  spaces are trimmed.
- `--no-error-trace` builds emit no sites and no calls (`?("ctx")` calls
  `slang_error_wrap` directly), so traces are empty.

---

## 8. Panics

All panic functions are `noreturn`. Declare them with the LLVM `noreturn`
attribute and follow the call with `unreachable`.

| Function | Message (after `panic: `) |
|---|---|
| `slang_panic(const char *msg)` | `msg` (a C string; NULL gives an empty message) |
| `slang_panic_str(const slang_string *msg)` | the string |
| `slang_panic_error(const slang_error *err)` | `slang_error_message(err)` (for `panic(err)`) |
| `slang_panic_nil()` | `runtime error: invalid memory address or nil pointer dereference` |
| `slang_panic_bounds(int64 idx, int64 len)` | `runtime error: index out of range [idx] with length len` |
| `slang_panic_slice_bounds(int64 lo, int64 hi, int64 cap)` | `runtime error: slice bounds out of range [lo:hi] with capacity cap` |

Behaviour:

1. `stdout` is flushed, so output printed before the panic is not lost.
2. `panic: <msg>\n` is written to stderr.
3. Unless `SLANG_NO_BACKTRACE` is set, a blank line, then
   `backtrace (most recent call first):`, then `backtrace_symbols_fd` frames.
4. The process exits immediately with **exit code 2** (`_exit`). No `defer`, no
   destructors, no atexit handlers.

A panic in any thread kills the whole process. If two threads panic at the same
time, only the first message is printed. Out-of-memory in any runtime allocation
panics with `out of memory (...)`.

---

## 9. I/O

All console output goes through C stdio `stdout`, which is buffered (line-buffered
on a terminal). Output is flushed at `slang_runtime_shutdown`, `slang_exit`, on
panic, and before `slang_read_line`.

| Function | Output |
|---|---|
| `slang_print_str(s)` / `slang_print_cstr(const char *)` | the bytes as-is (NULL gives nothing) |
| `slang_print_int(int64)` / `slang_print_uint(uint64)` | decimal |
| `slang_print_float(double)` | same format as `slang_string_from_float` |
| `double slang_float32_shortest(float f)` | the double nearest to the shortest decimal that round-trips as the float32 `f` (NaN/Inf unchanged). The compiler passes a `float32` through it before `slang_print_float` / `slang_string_from_float`, so `float32(0.1)` prints `0.1` and `float32(1)/3` prints `0.33333334` (Go) |
| `slang_print_bool(bool)` | `true` or `false` |
| `slang_print_rune(int32)` | UTF-8 encoding |
| `slang_print_space()` / `slang_print_newline()` | `' '` / `'\n'` |
| `slang_println_{str,cstr,int,uint,float,bool,rune}(v)` | value plus `'\n'`, written atomically with respect to other threads |
| `slang_eprint_str(s)` / `slang_eprintln_str(s)` | to stderr |
| `slang_flush()` | `fflush(stdout)` |

To lower a variadic `println(a, b, c)`, emit `print_X(a)`, `print_space()`,
`print_X(b)`, `print_space()`, `print_X(c)`, `print_newline()`. For a single
argument, use `println_X`. An `error` argument prints `slang_error_message(err)`
(release it afterwards).

Files and stdin:

| Function | Semantics |
|---|---|
| `slang_string *slang_read_file(const slang_string *path, slang_error **err)` | whole file as an owned string. On failure it returns NULL and sets `*err` to an owned error such as `open <path>: No such file or directory`. On success `*err = NULL` |
| `slang_error *slang_write_file(path, const slang_string *data)` | creates or truncates the file and writes it. Returns NULL on success or an owned error |
| `slang_error *slang_write_file_bytes(path, const slang_slice *data)` | the same, for `[]byte` |
| `slang_string *slang_read_line(slang_error **err)` | next line of stdin without `\n` (a trailing `\r` is also removed). A last line with no newline is still returned. At EOF it returns NULL and sets `*err = slang_err_eof` (immortal, so releasing it is harmless). On a read error it returns an owned error |

These calls are blocking. DESIGN.md's asynchronous I/O is future work: it would be
built as `*_async` variants that return a `slang_task *`.

---

## 10. Tasks, async/await and the scheduler

### 10.1 Model

- **Scheduler**: N worker threads (N = online CPUs, or the `SLANG_WORKERS`
  environment variable) take jobs from one global FIFO queue. A job is a
  `(fn, arg)` pair. The scheduler starts in `slang_runtime_init`, or lazily on the
  first `slang_sched_post`.
- **Task** (`slang_task *`, an opaque refcounted object): a state (pending or done),
  a result slot of `result_size` bytes with optional `result_ops`, and a list of
  continuations. A task completes **exactly once**.
- **Continuation**: `(resume_fn, frame)`. When the task completes, every registered
  continuation is **posted to the scheduler queue** (in registration order). It is
  never run synchronously inside `slang_task_complete` or
  `slang_task_add_continuation`. As a result, resumption never happens on the
  completer's stack, and it may happen on any worker thread.
- `async` functions are LLVM coroutines (switched-resume ABI). When a coroutine
  awaits a pending task, it registers its own handle as a continuation and
  suspends. A worker later calls `slang_coro_resume(handle)`.

### 10.2 Functions

| Function | Semantics |
|---|---|
| `void slang_sched_post(slang_resume_fn fn, void *arg)` | runs `fn(arg)` on some worker, FIFO. `typedef void (*slang_resume_fn)(void *)` |
| `int64_t slang_sched_workers(void)` | N |
| `bool slang_sched_is_worker(void)` | whether the current thread is a worker |
| `void slang_coro_resume(void *frame)` | resumes an LLVM switched-resume coroutine. It is exactly what `llvm.coro.resume` lowers to: it calls the function pointer stored in the first word of the frame. Pass `@slang_coro_resume` as `resume_fn` and the coroutine handle as `frame` |
| `slang_task *slang_task_new(int64_t result_size, const slang_value_ops *ops)` | a new **pending** task, +1. `result_size` is `sizeof(T)` (0 for `Task<void>`). `ops` describes ownership of the result |
| `void slang_task_complete(slang_task *t, const void *result)` | stores the result (copies `result_size` bytes from `result`, which is borrowed: the task **retains** it with `ops`; `result` may be NULL for void tasks or a zero result), marks the task done, wakes blocking waiters, and posts the continuations. **Panics `task completed twice`** on a second call. After it returns, `t` may already have been freed by another thread unless the caller still owns a reference |
| `bool slang_task_is_done(const slang_task *t)` | acquire-load of the state |
| `bool slang_task_add_continuation(slang_task *t, slang_resume_fn fn, void *frame)` | atomically: if `t` is still pending, registers `(fn, frame)` and returns **true** (the caller must now suspend). If `t` is already done, registers nothing and returns **false** (the caller continues immediately). Each registration fires exactly once |
| `void *slang_task_result(const slang_task *t)` | **[borrowed]** pointer to the result slot, valid while `t` is alive. Panics `task result read before completion` if pending. To keep a refcounted result, copy and **retain** it |
| `void *slang_task_await(slang_task *t)` | **blocking** await: waits until `t` is done and returns the **[borrowed]** result slot. See 10.5 for where it may be used |
| `slang_task *slang_task_spawn(slang_task_fn fn, void *ctx)` | creates a task (8-byte POD result slot) and posts `fn(task, ctx)` to the pool. `typedef void (*slang_task_fn)(slang_task *task, void *ctx)`. `fn` must call `slang_task_complete(task, ...)`, either before returning or later. The job holds its own reference only **until `fn` returns**, so to complete later `fn` must `slang_retain(task)`. Returns +1 |
| `slang_task_spawn_ex(fn, ctx, result_size, result_ops)` | the same, with an explicit result type |
| `slang_task *slang_task_delay(int64_t ms)` | `task.Delay(ms)`: a `Task<void>` completed by a timer thread after `ms` milliseconds (monotonic clock). `ms <= 0` returns an already-completed task. +1 |
| `slang_task *slang_task_all(slang_task *const *tasks, int64_t n)` | `Task<void>` that completes once every task in the array has completed. `n == 0` returns a completed task. Read the individual results from the input tasks. +1 |
| `slang_task *slang_task_any(slang_task *const *tasks, int64_t n)` | task whose 8-byte result is the `int64_t` **index** of the first input to complete. `n` must be at least 1. +1 |

**Task ownership rules**

1. Whoever will call `slang_task_complete` must hold a reference to the task until
   that call returns. A pending task must never be freed: its continuations would
   be lost. All runtime producers (delay, all, any, mutex, channel, spawn) follow
   this rule.
2. The continuation list holds **no** references to anything. The awaiting
   coroutine holds its own reference to the awaited task (it is a local in its
   frame) and releases it after reading the result.
3. The task's result slot owns one reference to the result (via `ops`), which is
   released when the task is freed.

### 10.3 Lowering `async func` (the protocol)

For `async func f(params) T`, the compiler emits **one** LLVM function
`ptr @f(params) presplitcoroutine` (the ramp, which is also the coroutine). Calling
it **starts the task immediately**: the body runs synchronously on the caller's
thread until its first real suspension, as in C#. It then returns a `Task<T>`,
owned +1 by the caller.

```text
entry:
  task  = slang_task_new(sizeof(T), ops_T)         ; +1, returned to the caller
  slang_retain(task)                               ; +1 owned by the frame
  id    = llvm.coro.id(0, null, null, null)
  mem   = slang_alloc(llvm.coro.size.i64())
  hdl   = llvm.coro.begin(id, mem)
  ... body ...

; each `x = await e` (e : Task<U>):
  t = <evaluate e>                                 ; +1 (owned local in the frame)
  if slang_task_is_done(t) goto ready              ; fast path, optional
  save = llvm.coro.save(hdl)
  if !slang_task_add_continuation(t, @slang_coro_resume, hdl) goto ready
  s = llvm.coro.suspend(save, false)               ; must come IMMEDIATELY after
  switch s: 0 -> ready, 1 -> cleanup, default -> suspended
ready:
  x = load U from slang_task_result(t); retain x if U is a reference type
  slang_release(t)

; each `return v`:
  store v into a temporary; slang_task_complete(task, &tmp)  ; task retains v
  release v (the temporary's own reference) if T is a reference type
  slang_release(task)                              ; drop the frame's reference
  br cleanup                                       ; NO final suspend

cleanup:
  slang_free(llvm.coro.free(id, hdl))
  br suspended
suspended:
  llvm.coro.end(hdl, false, none)
  ret ptr task                                     ; resume clones turn this into ret void
```

Rules the compiler must respect:

- **Do not touch the frame after `slang_task_add_continuation` returns true.**
  Another worker may already be resuming the coroutine, because the task can
  complete at any moment and continuations are posted immediately. Between that
  call and `llvm.coro.suspend` there must be no frame accesses. LLVM's coroutine
  splitting supports this, since the code after the suspend point in the
  ramp/resume clones is just a `ret`. This is the same pattern clang uses for C++
  `await_suspend` returning `bool`.
- **No final suspend point.** Once the result is published with
  `slang_task_complete`, nobody ever resumes the frame again, so the coroutine
  destroys its own frame (`coro.free` + `slang_free`). Nothing outside the
  coroutine ever calls `llvm.coro.destroy`, and cancellation does not exist in 0.1.
- Use `@slang_coro_resume` as the resume function (or your own trampoline that
  calls `llvm.coro.resume`). Coroutine resumption is always performed by a worker
  thread.
- Frame memory comes from `slang_alloc` and is released with `slang_free`. The frame
  is not a refcounted object. The task owns the result, and the frame owns its
  locals.
- `await` on an expression that yields a fresh task (`await f()`): the temporary
  task is owned by the frame and released after the result is read.
- `await f()?` (`T!` result): read `{value, err}` from the result slot, retain
  `err` (and the value, if needed), release the task, then apply the usual `?`
  lowering.
- `defer` inside an async function: 0.1 recommendation is to run defers at the
  logical function exit, before `slang_task_complete`.

`tests/coro_lowering.ll` is a complete, working example. `make test` compiles it
with `clang -O2` and runs it. It contains two async functions (one awaiting
`task.Delay`, the other awaiting the first) and a `main` that awaits with
`slang_task_await`. Start from that file.

### 10.4 Other ways to create tasks

- `std.task.Spawn(fn)`: `slang_task_spawn[_ex]`. It runs `fn` on a worker.
- Wrapping a callback-style operation: `t = slang_task_new(...)`, retain `t` for
  the producer, and the producer later calls `slang_task_complete(t, &res)` and
  then `slang_release(t)`.

### 10.5 Blocking await (`slang_task_await`)

- **Main thread** (or any non-worker thread): blocks on a condition variable until
  the task is done. This is how a synchronous `main` waits for an async `main`
  (section 11) and how `await` works in non-async code, if the language allows it.
- **Worker thread**: it also blocks, but before blocking, if no worker is idle, the
  scheduler starts a **spare worker thread** so N runnable workers remain. Spares
  retire once the blocked worker is running again and they find no work. Blocking
  on a worker is therefore safe from deadlock, but expensive (one OS thread per
  blocked call). **Compiler-generated `async` code must never call
  `slang_task_await`.** It uses the continuation protocol in 10.3.
  `slang_mutex_lock` and `slang_chan_send`/`slang_chan_recv` are blocking
  conveniences built on `slang_task_await`, so the same rule applies to them.

### 10.6 Mutex

`slang_mutex *` is an opaque refcounted object. It is **async-aware**: waiters are
tasks, and unlocking hands ownership directly to the oldest waiter (FIFO, fair).
It is not re-entrant.

| Function | Semantics |
|---|---|
| `slang_mutex_new()` | unlocked mutex, +1 |
| `slang_task *slang_mutex_lock_async(m)` | `Task<void>` that completes once the caller **owns** the lock. It is already completed if the mutex was free. Lowering of `await m.Lock()`: await it with the 10.3 protocol, then release the task |
| `slang_mutex_lock(m)` | blocking lock (`lock_async` + `slang_task_await`), for non-async code |
| `bool slang_mutex_try_lock(m)` | takes the lock if it is free |
| `slang_mutex_unlock(m)` | releases the lock or hands it to the next waiter. Panics `unlock of unlocked mutex` |

### 10.7 Channels

`slang_chan *` is an opaque refcounted object (`Channel<T>`). The element type is
given by `elem_size` and `ops`. Elements are copied and retained on send.

| Function | Semantics |
|---|---|
| `slang_chan_new(elem_size, ops, capacity)` | `capacity == 0` creates an unbuffered (rendezvous) channel; otherwise it has a buffer of `capacity` elements. +1 |
| `slang_task *slang_chan_send_async(ch, const void *elem)` | `Task<void>` that completes once the value has been handed to a receiver or buffered. Panics `send on closed channel` |
| `slang_task *slang_chan_recv_async(ch)` | task whose result slot has this layout: **`{ int64_t ok @0; T value @8 }`** (`result_size = 8 + elem_size`). `ok = 1` means a value was received, and the slot owns it (copy and retain it). `ok = 0` means the channel is closed and drained, and the value is all zero |
| `slang_chan_send(ch, elem)` | blocking send |
| `bool slang_chan_recv(ch, void *out)` | blocking receive. Writes an **owned** copy to `*out` (the caller releases it) and returns `ok` |
| `slang_chan_close(ch)` | wakes all pending receivers with `ok = 0`. Panics `close of closed channel`, and panics `send on closed channel` if senders are blocked |
| `int64_t slang_chan_len(ch)` | number of buffered elements |

Buffered values are released when the channel is freed.

(The Slang `std.task.Channel<T>` and `std.task.Mutex` are written in Slang on
top of the primitives of 10.10, not on `slang_chan`/`slang_mutex`, because their
zero values must work without a constructor and the element type is only known
to the compiler. `slang_chan`/`slang_mutex` remain for C code and tests.)

### 10.8 Task context

Every thread has a **current context**: one owned `slang_object *` (NULL =
empty). The runtime never looks inside it. `std.task` stores a one-entry
`map[string]any` there whose value is the head of an immutable linked list of
frames `ctxFrame{key int, val any, next *ctxFrame}`: `task.With(key, value, fn)`
pushes one frame for the duration of `fn`, `task.Get(key)` walks the list,
inheritance is copying the pointer. Keys are `task.Key<T>()` (ids from
`int64_t slang_task_next_key(void)`). Cancellation lives alongside, in the task
tree of 10.11.

| Function | Semantics |
|---|---|
| `slang_object *slang_task_get_ctx(void)` | the current context, **+1** (NULL if empty) |
| `void slang_task_set_ctx(slang_object *ctx)` | makes `ctx` (borrowed; retained) current and releases the previous one |

Propagation, with no help from compiled code:

- `slang_task_add_continuation` records the registering thread's context in the
  continuation (retained); the job that resumes it runs with that context. So an
  `async` function sees, after every `await`, the context it had when it
  suspended, on whatever worker resumes it.
- `slang_sched_post` (and so `slang_task_spawn[_ex]`) captures the poster's
  context for the job.
- A worker sets the job's context before calling `fn(arg)` and releases whatever
  is current when `fn` returns, so contexts never leak between jobs.
- Calling an `async` function starts it on the caller's thread, so it inherits
  the caller's context. `std.task.RunWith(ctx, f)` sets `ctx`, calls `f`, and
  restores the previous context before returning `f`'s task.

### 10.9 I/O readiness waits (`src/task_io.c`)

One I/O thread (started lazily, stopped by `slang_runtime_shutdown` after the
timers; pending waits are then released without completing) runs the platform
poller (epoll on Linux, section 15). A wait is a `Task<int>` completed by the I/O
thread, which posts the continuations to the scheduler exactly like a timer.

| Function | Semantics |
|---|---|
| `slang_task *slang_io_wait_readable(int64_t fd, int64_t timeout_ms)` | +1 task whose `int64_t` result is **0** when `fd` is readable (or at end of stream / error), **1** when `timeout_ms` (>= 0) elapsed first, **2** when `slang_io_forget(fd)` was called. `timeout_ms < 0`: no timeout |
| `slang_task *slang_io_wait_writable(int64_t fd, int64_t timeout_ms)` | the same for writability |
| `void slang_io_forget(int64_t fd)` | completes pending waits on `fd` with 2 and unregisters it. **Call it before `close(fd)`** (a reused descriptor number must not inherit waits) |

Rules: `fd` must be non-blocking. At most one reader and one writer may wait on a
descriptor at a time (a second one panics `two tasks wait to read file descriptor
N at the same time`). The poller is level-triggered and an fd is registered only
while somebody waits on it, so a wait completes at once when data is already
there. A descriptor that cannot be polled (a regular file) is always ready.

### 10.10 Primitives for the Slang-source `std.task`

| Function | Semantics |
|---|---|
| `bool slang_task_try_complete(slang_task *t, const void *result)` | `slang_task_complete` that returns false instead of panicking when `t` is already done |
| `slang_task *slang_task_rt_signal(void)` | a pending `Task<void>`, +1 (a "signal"), fired early when the current task is cancelled (10.11) |
| `slang_task *slang_task_rt_waker(void)` | the same, but never fired by cancellation (`Mutex`, `WaitGroup`) |
| `void slang_task_rt_fire(slang_task *t)` | completes `t` if it is still pending; if `t` is a pending `slang_task_delay` timer, the timer is cancelled first (used by `task.Timeout`) |
| `void slang_task_spin_lock(int64_t *slot)` / `slang_task_spin_unlock` | a test-and-set spin lock on an `int64_t` (0 = free, 1 = held; spins, then yields the CPU). Slang code passes `&this.guard` of an `int` field. Hold it only for a few statements, never across an `await` |
| `slang_task *slang_task_yield(void)` | `task.Yield()`: +1 `Task<void>` completed by a fresh job, so awaiting it reschedules the coroutine (on any worker) |

`task.Delay`, `task.All`, `task.Any` and `task.Yield` are runtime functions; the
rest of `std.task` is Slang (`compiler/src/sema/stdlib_src/task.sl`):

- `Mutex`: `{ guard int; locked bool; waiters []Task<void> }`. `Lock()` returns a
  completed task if the mutex was free, else a signal queued in `waiters`;
  `Unlock()` hands ownership to the oldest waiter by firing its signal (FIFO).
- `Channel<T>`: a handle to a heap `chanState<T>` (copies share it) with the
  values in a `[]T` and waiting receivers/senders as signals; waiters re-check
  their condition after waking, so wake-ups are never lost (a signal fired
  before it is awaited is simply already complete). Unbuffered channels admit
  one value at a time and the sender waits until a receiver took it.
- `WaitGroup`, `Spawn`/`Go` (`await task.Yield()` then call), `Timeout` (a delay
  that a watcher task cancels with `slang_task_rt_fire` when the watched task
  finishes), `Key`/`Get`/`With`/`CurrentContext`/`RunWith`/`SpawnWith` (10.8),
  `NewCancel`/`WithTimeout`/`CheckCancelled`/`IsCancelled`/`ErrCancelled` (10.11).

### 10.11 Cancellation and the current task

**The task tree.** Every thread has a *current task* (owned, NULL = none),
carried by jobs and continuations exactly like the context of 10.8.

- `slang_task_new` (the coroutine ramp's call) makes the new task a **child of
  the current task** and **current** itself. The current task becomes its parent
  again when the coroutine suspends (`slang_task_add_continuation` returning
  true) or completes (`slang_task_complete` of the current task); after that the
  code running on the thread is the caller's. So `async` calls form a tree with
  no help from the compiler. Runtime-made tasks (delays, signals, waits, `All`,
  `Any`, spawns) use `slang_task_new_plain`, which does neither;
  **C code that makes a task for a callback must use `slang_task_new_plain`**.
- A child holds a reference to its parent until it is freed; the parent lists
  its children without owning them.

| Function | Semantics |
|---|---|
| `void slang_task_cancel(slang_task *t)` | sets the cancelled flag of `t` and of all its descendants (tasks created later under a cancelled task start cancelled) and runs their **cancel hooks** |
| `bool slang_task_is_cancelled(const slang_task *t)` / `bool slang_task_cur_cancelled(void)` | the flag of `t` / of the current task (`task.IsCancelled()`) |
| `void slang_task_set_deadline(slang_task *t, int64_t ms)` | the timer thread cancels `t` after `ms` milliseconds (`task.WithTimeout`) |
| `slang_task *slang_task_current(void)` | the current task, +1 (or NULL) |
| `slang_task *slang_task_new_scope(void)` | a plain task attached under the current task, used only as a cancellation node (`task.NewCancel`), +1 |
| `slang_task *slang_task_enter_scope(slang_task *scope)` / `void slang_task_leave_scope(slang_task *prev)` | make `scope` current (NULL keeps the current task) and return the previous one (+1); restore it (`task.RunWith`) |
| `slang_error *slang_task_err_cancelled(void)` | `task.ErrCancelled`, an immortal sentinel (`errors.Is`) with message `task cancelled` |

**Cancel hooks** wake the waits of a cancelled task: `slang_task_delay`
(completes at once), `slang_task_rt_signal` (fired, so `Channel.Send/Recv`
re-check and give up), `slang_io_wait_*` (complete with **3**, so `std.net`
operations fail with `task.ErrCancelled`). A wait started by an already
cancelled task completes immediately. `task.Any` cancels its losing inputs.

Limits (the compiler is not involved, so an `await` cannot be interrupted by
itself): awaiting another task's result waits until that task finishes (it
finishes early only if it reacts to the cancellation); `Delay`, `Channel.Send`
and other `Task<void>` waits wake early but cannot carry the error, so
cancellable loops call `task.CheckCancelled()?`; `Mutex.Lock` and
`WaitGroup.Wait` are not cancellable; `task.All` does not cancel the others on
the first error; there is no `task.Scope`, `WithDeadline` or `OnCancel` yet.

---

## 11. Program entry

The runtime does **not** define `main`. The compiler emits it:

```llvm
define i32 @main(i32 %argc, ptr %argv) {
  call void @slang_runtime_init(i32 %argc, ptr %argv)
  ; synchronous `func main()`:
  call void @app.main()
  ; or, for `async func main()`:
  ;   %t = call ptr @app.main()           ; the ramp runs until the first await
  ;   call ptr @slang_task_await(ptr %t)  ; main thread blocks
  ;   call void @slang_release(ptr %t)
  call void @slang_runtime_shutdown()
  ret i32 0
}
```

For `func main() void!` the call returns the error (`ptr`; for `async` it
is read, and retained, from the awaited task's result slot). After
`slang_runtime_shutdown` a non-NULL error is passed to `slang_error_report`
and released, and `main` returns 1.

| Function | Semantics |
|---|---|
| `void slang_runtime_init(int argc, char **argv)` | stores the arguments and starts the worker pool and the timer thread. Idempotent. Call it first, on the main thread |
| `void slang_runtime_shutdown(void)` | waits until the job queue is empty and no job is running, stops the workers and the timer thread (pending timers are dropped without firing), flushes stdio, and frees the argument slice. Call it from the main thread only (calling it from a worker deadlocks). After shutdown the scheduler restarts lazily if used again |
| `void slang_runtime_drain(void)` | the first step of `slang_runtime_shutdown` alone: waits until no job is queued or running and stops the workers and the timer thread. The compiler calls it after `main` returns, then releases the program's globals (their destructors may print or even start tasks again), then calls `slang_runtime_shutdown` |
| `void slang_os_args(slang_slice *out)` | `os.Args`: an owned `[]string` (`elem_size` 8, ops `slang_ops_ref`) that **shares** the runtime's backing array. Treat it as read-only |
| `void slang_exit(int32_t code)` | flushes stdout and stderr, then calls `_Exit(code)` immediately (no defers, no shutdown wait). `noreturn` |

Returning from `main` without calling `slang_runtime_shutdown` is allowed, but
output still sitting in buffers may be lost if workers are still printing.

---

## 12. Environment variables

| Variable | Effect |
|---|---|
| `SLANG_WORKERS=n` | number of worker threads (default: online CPUs, max 256) |
| `SLANG_NO_BACKTRACE=1` | panic prints only the `panic: ...` line (useful for golden-output tests) |
| `SLANG_NO_SLAB=1` | objects come straight from `malloc` instead of the small-object allocator (3.2), e.g. so Valgrind memcheck sees each object |

---

## 13. Known limits in v0.1

- Value alignment is limited to 8 bytes (slice data is 16-aligned; map values and
  task results are only 8-aligned). Do not store `long double` or 16-byte SIMD
  types in containers.
- There is no cycle collection (use weak references, 3.4; `--leak-check`
  reports what a cycle kept alive).
- Map iteration order is deterministic but unspecified. Mutating a map during
  iteration is only partly supported (6.3).
- `errors.Is` compares identity only (7.2).
- File I/O and `std.io` are blocking. Network I/O is asynchronous (10.9, 16),
  but there is no `select` over channels and no task cancellation.
- The I/O poller exists for Linux (epoll) only; the kqueue (macOS/BSD) and IOCP
  (Windows) backends are not written yet (15).
- The scheduler uses a single global queue (no work stealing). Blocking awaits on
  workers start spare threads.
- Panics do not unwind: no `defer`, no cleanup.
- Strings are byte strings: indexing, slicing and `len` work on bytes. Rune helpers
  are `slang_string_rune_count`, `slang_string_decode_rune` and
  `slang_string_from_rune`.

---

## 14. Standard library

The `std.*` functions that `compiler/src/sema/stdlib.rs` lowers to externs.
Every symbol is `slang_<ns>_<snake_name>`. Implemented in `src/std_*.c`,
declared in the "Standard library" section of `slang_rt.h`.

### 14.1 Calling convention

| Slang type | C parameter | C result |
|---|---|---|
| `string` | `const slang_string *` (borrowed, NULL = `""`) | `slang_string *`, owned (+1), never NULL |
| `int` / `float` / `bool` | `int64_t` / `double` / `bool` | same |
| `[]T` | `const slang_slice *` (borrowed, may be NULL = empty) | **trailing** out-param `slang_slice *out`, function returns `void`. `*out` is overwritten without being released and receives an owned slice |
| `error` | `const slang_error *` (borrowed, NULL = nil) | `slang_error *`, owned (+1) or NULL |
| `T!` | | `T` is returned, plus a **trailing** `slang_error **err`: `*err = NULL` on success; on failure `*err` is an owned error and the returned value is the zero value (0 / NULL). `err` may be NULL to discard the error |
| `void!` | | `void` return plus the same trailing `slang_error **err` |
| `Task<void>` | | `slang_task *`, owned (+1) |

`[]string` slices produced or consumed here use `elem_size` 8 and ops
`&slang_ops_ref`; elements are `slang_string *` slots, NULL elements mean `""`.
A string result that equals an argument may be **the argument object itself**,
retained (strings are immutable); the caller just releases it as usual.

### 14.2 Signatures

| Slang | C signature | Semantics / notes |
|---|---|---|
| `strings.Split(s, sep)` | `void slang_strings_split(const slang_string *s, const slang_string *sep, slang_slice *out)` | Go `strings.Split`: n+1 pieces for n non-overlapping matches; `Split("", ",")` = `[""]`; empty `sep` splits after each UTF-8 sequence (an invalid byte is its own 1-byte piece) and `Split("", "")` = `[]` |
| `strings.Join(parts, sep)` | `slang_string *slang_strings_join(const slang_slice *parts, const slang_string *sep)` | empty/NULL `parts` gives `""` |
| `strings.Trim(s)` | `slang_string *slang_strings_trim(const slang_string *s)` | Go `strings.TrimSpace`: strips leading/trailing Unicode white space (`\t \n \v \f \r` space, U+0085, U+00A0, U+1680, U+2000..U+200A, U+2028, U+2029, U+202F, U+205F, U+3000). No cutset |
| `strings.Contains(s, sub)` | `bool slang_strings_contains(const slang_string *s, const slang_string *substr)` | `Contains(s, "")` is true |
| `strings.Replace(s, old, new)` | `slang_string *slang_strings_replace(const slang_string *s, const slang_string *old, const slang_string *new_)` | replaces **all** non-overlapping occurrences, left to right (Go `ReplaceAll`). Empty `old` inserts `new` at the start and after each UTF-8 sequence (`Replace("ab", "", "-")` = `-a-b-`) |
| `strings.ToUpper(s)` / `ToLower(s)` | `slang_string *slang_strings_to_upper(const slang_string *s)` / `slang_strings_to_lower` | simple 1:1 case mapping for ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic (U+0400..U+04BF); other code points (e.g. `ß`) and invalid bytes are copied unchanged (Go would also map other scripts, and turns invalid bytes into U+FFFD) |
| `strings.HasPrefix(s, p)` / `HasSuffix(s, p)` | `bool slang_strings_has_prefix(const slang_string *s, const slang_string *prefix)` / `bool slang_strings_has_suffix(const slang_string *s, const slang_string *suffix)` | |
| `strings.Index(s, sub)` | `int64_t slang_strings_index(const slang_string *s, const slang_string *substr)` | **byte** offset of the first match, -1 if none, 0 for empty `sub` |
| `strings.Repeat(s, n)` | `slang_string *slang_strings_repeat(const slang_string *s, int64_t count)` | panics `strings: negative Repeat count` if `count < 0` and `strings: Repeat output length overflow` if the result would not fit |
| `strings.ToInt(s)` | `int64_t slang_strings_to_int(const slang_string *s, slang_error **err)` | same as `slang_string_to_int` (4.2): base 10, optional `+`/`-`, no whitespace; errors `invalid integer: "..."` / `integer out of range: "..."` |
| `strings.ToFloat(s)` | `double slang_strings_to_float(const slang_string *s, slang_error **err)` | same as `slang_string_to_float`; `1e400` is an error (`float out of range`) |
| `math.Sqrt/Abs/Floor/Ceil/Sin/Cos/Tan/Log/Exp(x)` | `double slang_math_sqrt(double x)` (and `_abs`, `_floor`, `_ceil`, `_sin`, `_cos`, `_tan`, `_log`, `_exp`) | C libm; IEEE results (`Sqrt(-1)` NaN, `Log(0)` -Inf) |
| `math.Pow(x, y)` | `double slang_math_pow(double x, double y)` | C `pow` |
| `math.Min(x, y)` / `Max(x, y)` | `double slang_math_min(double x, double y)` / `double slang_math_max(double x, double y)` | Go semantics, not C `fmin`: `Min(x, -Inf)` = -Inf and `Max(x, +Inf)` = +Inf even with NaN, otherwise any NaN gives NaN; `Min(-0, +0)` = -0, `Max(-0, +0)` = +0 |
| `fmt.Format(tmpl, args...)` | `slang_string *slang_fmt_format(const slang_string *tmpl, const slang_slice *args)` | `args` is a `[]string` of the arguments already formatted as `println` would (the compiler does the conversion). Each `{N}` (N = ASCII digits, leading zeros allowed) with `N < len(args)` is replaced by `args[N]`; a placeholder whose index is out of range is **kept verbatim** (`{2}`); extra arguments are ignored; any other text, including `{`, `}`, `{}`, `{x}`, `$`, is copied verbatim (no `{{` escaping). `args` may be NULL |
| `io.ReadFile(path)` | `slang_string *slang_io_read_file(const slang_string *path, slang_error **err)` | = `slang_read_file` (9) |
| `io.WriteFile(path, data)` | `void slang_io_write_file(const slang_string *path, const slang_string *data, slang_error **err)` | `slang_write_file` with the error moved to `*err` (`void!` convention); with `err == NULL` the error is released |
| `io.ReadLine()` | `slang_string *slang_io_read_line(slang_error **err)` | = `slang_read_line`: at EOF returns NULL and `*err = slang_err_eof` (immortal; releasing it is harmless) |
| `errors.Wrap(err, msg)` | `slang_error *slang_errors_wrap(const slang_error *err, const slang_string *msg)` | = `slang_error_wrap`: new error, `Message()` = `msg + ": " + err.Message()`, cause `err` retained. **`err == NULL` returns NULL** (wrapping nil is nil) |
| `errors.Unwrap(err)` | `slang_error *slang_errors_unwrap(const slang_error *err)` | = `slang_error_unwrap`: the cause, owned (+1), or NULL if `err` is not a wrapper (or is NULL) |
| `errors.Is(err, target)` | `bool slang_errors_is(const slang_error *err, const slang_error *target)` | = `slang_error_is`: identity along the chain (7.2) |
| `task.Delay(ms)` | `slang_task *slang_task_delay(int64_t ms)` | the existing function (10.2); no alias needed |

Not externs (lowered by the compiler, see `stdlib.rs` specials): `errors.New`
-> `slang_error_new`, `errors.As` -> `slang_error_as`, `task.All` ->
`slang_task_all`, `println`/`print` -> `slang_print_*`. `math.Pi` and `math.E`
are inlined constants.

### 14.3 Second batch: time, os, fs, random, crypto, encoding, uuid

Same conventions as 14.1. Additional slice element layouts: `[]byte` is
`elem_size` 1 and `[]char` is `elem_size` 4 (`int32_t` code points), both with
NULL ops; `[]int` is `elem_size` 8 with NULL ops. Errors from the OS have the
message `"<op> <path>: <strerror(errno)>"` (e.g. `mkdir data: File exists`).
Paths and other strings with an embedded NUL byte are rejected with `EINVAL`
(or treated as non-existent by `Exists`/`IsDir`).

**std.strings** (`src/std_strings_ext.c`; byte offsets, invalid UTF-8 bytes count as one rune = U+FFFD)

| Slang | C signature | Semantics / notes |
|---|---|---|
| `Fields(s) []string` | `void slang_strings_fields(const slang_string *s, slang_slice *out)` | Go `strings.Fields`: split around runs of Unicode white space (same set as `Trim`); `[]` for blank input |
| `Count(s, sub) int` | `int64_t slang_strings_count(const slang_string *s, const slang_string *substr)` | non-overlapping count; empty `sub` gives rune count + 1 |
| `LastIndex(s, sub) int` | `int64_t slang_strings_last_index(const slang_string *s, const slang_string *substr)` | byte offset of the last match, -1 if none, `len(s)` for empty `sub` |
| `TrimPrefix(s, p)` / `TrimSuffix(s, p)` | `slang_string *slang_strings_trim_prefix(const slang_string *s, const slang_string *prefix)` / `_trim_suffix` | removes one occurrence if present |
| `TrimLeft(s, cutset)` / `TrimRight(s, cutset)` | `slang_string *slang_strings_trim_left(const slang_string *s, const slang_string *cutset)` / `_trim_right` | Go semantics: strips leading/trailing runes contained in `cutset`; an invalid byte is stripped only if the same invalid byte is in `cutset` |
| `SplitN(s, sep, n) []string` | `void slang_strings_split_n(const slang_string *s, const slang_string *sep, int64_t n, slang_slice *out)` | Go: `n > 0` at most `n` pieces (last is the rest), `n == 0` gives `[]`, `n < 0` = `Split` |
| `EqualFold(a, b) bool` | `bool slang_strings_equal_fold(const slang_string *a, const slang_string *b)` | simple case folding over the `ToUpper`/`ToLower` ranges (`lower(upper(x))`, so `ς`/`σ`/`Σ` match) |
| `Reverse(s)` | `slang_string *slang_strings_reverse(const slang_string *s)` | reverses code points (invalid bytes kept as single units) |
| `PadLeft(s, width, pad)` / `PadRight` | `slang_string *slang_strings_pad_left(const slang_string *s, int64_t width, const slang_string *pad)` / `_pad_right` | pads to `width` **runes** by cycling the runes of `pad`; unchanged if already `>= width` or `pad == ""` |
| `Runes(s) []char` | `void slang_strings_runes(const slang_string *s, slang_slice *out)` | decoded code points, U+FFFD for invalid bytes |
| `FromRunes(r []char)` | `slang_string *slang_strings_from_runes(const slang_slice *runes)` | UTF-8 encoding; surrogates, negatives and > U+10FFFF become U+FFFD |
| `Bytes(s) []byte` / `FromBytes(b)` | `void slang_strings_bytes(const slang_string *s, slang_slice *out)` / `slang_string *slang_strings_from_bytes(const slang_slice *bytes)` | = `slang_string_to_bytes` / `slang_string_from_bytes` (copies) |
| `Compare(a, b) int` | `int64_t slang_strings_compare(const slang_string *a, const slang_string *b)` | -1 / 0 / 1, byte-wise |
| `Title(s)` | `slang_string *slang_strings_title(const slang_string *s)` | upper-cases the first rune of each white-space separated word (Go's deprecated `strings.Title`, white space as the only separator) |

**std.math** (`src/std_math_ext.c`). Slang has no overloading, so the int
versions of `Min`/`Max`/`Abs` (which stay `float`) are `IMin`/`IMax`/`IAbs`.
`MaxInt`/`MinInt` constants are not provided yet: the stdlib constant table
only holds untyped floats (`Pi`, `E`), and `2^63-1` is not representable.

| Slang | C signature | Semantics / notes |
|---|---|---|
| `Round/Trunc/Log2/Log10(x float) float` | `double slang_math_round(double x)` (and `_trunc`, `_log2`, `_log10`) | libm; `Round` rounds half away from zero (Go) |
| `Mod(x, y)` / `Hypot(x, y)` / `Atan2(y, x)` | `double slang_math_mod(double x, double y)` / `slang_math_hypot` / `double slang_math_atan2(double y, double x)` | `fmod` (sign of `x`), `hypot`, `atan2` |
| `IsNaN(x) bool` | `bool slang_math_is_nan(double x)` | |
| `IsInf(x, sign int) bool` | `bool slang_math_is_inf(double x, int64_t sign)` | Go: `sign > 0` +Inf only, `< 0` -Inf only, `0` either |
| `Inf(sign int) float` / `NaN() float` | `double slang_math_inf(int64_t sign)` / `double slang_math_nan(void)` | `Inf(sign >= 0)` = +Inf |
| `IMin(a, b)` / `IMax(a, b)` / `IAbs(a)` (int) | `int64_t slang_math_imin(int64_t a, int64_t b)` / `_imax` / `int64_t slang_math_iabs(int64_t a)` | `IAbs(MinInt)` wraps to `MinInt` |

**std.io** (`src/std_io_ext.c`)

| Slang | C signature | Semantics / notes |
|---|---|---|
| `ReadAll() string!` | `slang_string *slang_io_read_all(slang_error **err)` | stdin (the same `FILE *` as `ReadLine`) to EOF; at EOF returns `""` (not an error) |
| `ReadLines(path) []string!` | `void slang_io_read_lines(const slang_string *path, slang_slice *out, slang_error **err)` | lines without `\n`, a trailing `\r` is dropped, a final newline adds no empty line; errors as `ReadFile` |
| `WriteLines(path, lines []string) void!` | `void slang_io_write_lines(const slang_string *path, const slang_slice *lines, slang_error **err)` | each line followed by `\n`; truncates/creates like `WriteFile` |
| `Eprint(s)` / `Eprintln(s)` | `void slang_io_eprint(const slang_string *s)` / `slang_io_eprintln` | to stderr (stdout is flushed first); one `string` argument, not variadic |

**std.time** (`src/std_time.c`): instants are `int` Unix **milliseconds**; calendar accessors are UTC.

| Slang | C signature | Semantics / notes |
|---|---|---|
| `Now() int` / `NowNanos() int` | `int64_t slang_time_now(void)` / `slang_time_now_nanos` | `CLOCK_REALTIME` |
| `Sleep(ms int)` | `void slang_time_sleep(int64_t ms)` | blocks the calling thread (`nanosleep`, restarted on EINTR); `ms <= 0` returns at once. In async code prefer `await task.Delay(ms)` |
| `Since(startMs int) int` | `int64_t slang_time_since(int64_t start_ms)` | `Now() - startMs` |
| `Format(ms, layout) string` | `slang_string *slang_time_format(int64_t ms, const slang_string *layout)` | `strftime` in UTC, C locale, plus `%L` = 3-digit milliseconds; negative times floor to the previous second |
| `Parse(s, layout) int!` | `int64_t slang_time_parse(const slang_string *s, const slang_string *layout, slang_error **err)` | `strptime`, UTC unless the layout has `%z` (offset applied); the whole input must match; fields not in the layout default to 1970-01-01 00:00:00; error `time: cannot parse "<s>" as "<layout>"` |
| `Year/Month/Day/Hour/Minute/Second(ms) int` | `int64_t slang_time_year(int64_t ms)` (and `_month` 1..12, `_day` 1..31, `_hour`, `_minute`, `_second`) | UTC |
| `Weekday(ms) int` | `int64_t slang_time_weekday(int64_t ms)` | 0 = Sunday ... 6 = Saturday |

**std.os** (`src/std_os.c`)

| Slang | C signature | Semantics / notes |
|---|---|---|
| `Args() []string` | `void slang_os_args(slang_slice *out)` | existing function (11): program name first |
| `Getenv(name) string` | `slang_string *slang_os_getenv(const slang_string *name)` | `""` if unset |
| `Setenv(name, value) void!` | `void slang_os_setenv(const slang_string *name, const slang_string *value, slang_error **err)` | EINVAL for an empty name or one containing `=` |
| `Exit(code int)` | `SLANG_NORETURN void slang_os_exit(int64_t code)` | = `slang_exit((int32_t)code)`: flushes stdout/stderr, no defers run |
| `Hostname() string!` / `Cwd() string!` | `slang_string *slang_os_hostname(slang_error **err)` / `slang_os_cwd` | `gethostname` / `getcwd` |
| `Pid() int` | `int64_t slang_os_pid(void)` | |
| `Chdir(path) void!` | `void slang_os_chdir(const slang_string *path, slang_error **err)` | process-wide |

**std.fs** (`src/std_fs.c`). Path functions are pure string operations on `/`-separated paths with Go `path/filepath` (Unix) semantics.

| Slang | C signature | Semantics / notes |
|---|---|---|
| `Exists(path) bool` / `IsDir(path) bool` | `bool slang_fs_exists(const slang_string *path)` / `slang_fs_is_dir` | `stat` (follows symlinks); false on any error |
| `ReadDir(path) []string!` | `void slang_fs_read_dir(const slang_string *path, slang_slice *out, slang_error **err)` | entry names without `.`/`..`, sorted byte-wise |
| `Mkdir(path) void!` / `MkdirAll(path) void!` | `void slang_fs_mkdir(const slang_string *path, slang_error **err)` / `_mkdir_all` | mode 0777 & umask; `MkdirAll` creates parents, succeeds if the directory exists, fails with ENOTDIR through a file |
| `Remove(path) void!` | `void slang_fs_remove(const slang_string *path, slang_error **err)` | file or empty directory (`remove(3)`) |
| `RemoveAll(path) void!` | `void slang_fs_remove_all(const slang_string *path, slang_error **err)` | recursive, does not follow symlinks; a missing path (or `""`) is success |
| `Rename(from, to) void!` | `void slang_fs_rename(const slang_string *from, const slang_string *to, slang_error **err)` | `rename(2)` |
| `Size(path) int!` / `ModTime(path) int!` | `int64_t slang_fs_size(const slang_string *path, slang_error **err)` / `_mod_time` | bytes / mtime in Unix ms |
| `Copy(from, to) void!` | `void slang_fs_copy(const slang_string *from, const slang_string *to, slang_error **err)` | file contents only; `to` is created (0666 & umask) or truncated; a directory source is EISDIR |
| `AppendFile(path, data) void!` | `void slang_fs_append_file(const slang_string *path, const slang_string *data, slang_error **err)` | `O_APPEND`, creates the file |
| `Join(a, b)` | `slang_string *slang_fs_join(const slang_string *a, const slang_string *b)` | joins non-empty parts with `/` and cleans (`Join("a/b", "../c")` = `a/c`); `Join("", "")` = `""` |
| `Base(p)` / `Dir(p)` / `Ext(p)` | `slang_string *slang_fs_base(const slang_string *path)` / `_dir` / `_ext` | Go: `Base("")` = `.`, `Base("a/b/")` = `b`; `Dir("a")` = `.`, cleaned; `Ext` from the last `.` of the last element, `""` if none |
| `Abs(p) string!` | `slang_string *slang_fs_abs(const slang_string *path, slang_error **err)` | cleaned; relative paths are joined to the cwd |
| `TempDir() string` | `slang_string *slang_fs_temp_dir(void)` | `$TMPDIR` or `/tmp` |

**std.random** (`src/std_random.c`): xoshiro256** seeded through splitmix64,
one process-wide state behind a mutex; seeded from `getrandom(2)` on first use
unless `Seed` is called, after which the sequence is fixed. Not for secrets.

| Slang | C signature | Semantics / notes |
|---|---|---|
| `Seed(n int)` | `void slang_random_seed(int64_t seed)` | |
| `Int() int` | `int64_t slang_random_int(void)` | uniform in `[0, 2^63)` |
| `IntN(n int) int` | `int64_t slang_random_int_n(int64_t n)` | uniform in `[0, n)` (rejection sampling); panics `random: invalid argument to IntN` if `n <= 0` |
| `Float() float` | `double slang_random_float(void)` | 53 random bits in `[0, 1)` |
| `Bytes(n int) []byte` | `void slang_random_bytes(int64_t n, slang_slice *out)` | panics if `n < 0` |
| `Shuffle(s []int)` | `void slang_random_shuffle(const slang_slice *s)` | in-place Fisher-Yates through the shared buffer; only `[]int` (no generics yet) |

**std.crypto.hash / std.crypto.hmac / std.crypto.rand** (`src/std_crypto.c`):
self-contained implementations of FIPS 180-4 (SHA-1/256/512), RFC 1321 (MD5),
RFC 2104 (HMAC) and IEEE CRC-32. Inputs are the bytes of the string; hex is lowercase.

| Slang | C signature | Semantics / notes |
|---|---|---|
| `hash.Sha256(data) []byte` / `Sha256Hex(data) string` | `void slang_crypto_hash_sha256(const slang_string *data, slang_slice *out)` / `slang_string *slang_crypto_hash_sha256_hex(const slang_string *data)` | 32 bytes / 64 hex chars |
| `hash.Sha512` / `Sha512Hex` | `slang_crypto_hash_sha512` / `_sha512_hex` (same shapes) | 64 bytes |
| `hash.Sha1` / `Sha1Hex`, `hash.Md5` / `Md5Hex` | `slang_crypto_hash_sha1` / `_sha1_hex`, `slang_crypto_hash_md5` / `_md5_hex` | legacy, for compatibility only |
| `hash.Crc32(data) int` | `int64_t slang_crypto_hash_crc32(const slang_string *data)` | IEEE (zlib, Go `crc32.ChecksumIEEE`), `0..2^32-1` |
| `hmac.Sha256(key, data) []byte` / `Sha256Hex` | `void slang_crypto_hmac_sha256(const slang_string *key, const slang_string *data, slang_slice *out)` / `slang_string *slang_crypto_hmac_sha256_hex(...)` | HMAC-SHA256 (RFC 4231 vectors in tests) |
| `hmac.Equal(a, b []byte) bool` | `bool slang_crypto_hmac_equal(const slang_slice *a, const slang_slice *b)` | time independent of the contents (lengths may leak) |
| `rand.Bytes(n) []byte!` | `void slang_crypto_rand_bytes(int64_t n, slang_slice *out, slang_error **err)` | `getrandom(2)`, falling back to `/dev/urandom`; `n < 0` is an error |
| `rand.Int() int!` | `int64_t slang_crypto_rand_int(slang_error **err)` | uniform in `[0, 2^63)` |
| `rand.Hex(n) string!` | `slang_string *slang_crypto_rand_hex(int64_t n, slang_error **err)` | `n` random bytes as `2n` hex chars |

**std.encoding.base64 / std.encoding.hex** (`src/std_encoding.c`, RFC 4648)

| Slang | C signature | Semantics / notes |
|---|---|---|
| `base64.Encode(b []byte)` / `EncodeString(s)` | `slang_string *slang_encoding_base64_encode(const slang_slice *data)` / `_encode_string(const slang_string *s)` | standard alphabet, `=` padding |
| `base64.Decode(s) []byte!` / `DecodeString(s) string!` | `void slang_encoding_base64_decode(const slang_string *s, slang_slice *out, slang_error **err)` / `slang_string *slang_encoding_base64_decode_string(const slang_string *s, slang_error **err)` | padding required; `\r`/`\n` skipped; unused trailing bits ignored (Go non-strict); error `illegal base64 data at input byte N` |
| `base64.EncodeURL(b)` / `DecodeURL(s) []byte!` | `slang_string *slang_encoding_base64_encode_url(const slang_slice *data)` / `void slang_encoding_base64_decode_url(const slang_string *s, slang_slice *out, slang_error **err)` | URL-safe alphabet (`-_`), **no padding** on output (Go `RawURLEncoding`); the decoder accepts padded or unpadded input |
| `hex.Encode(b)` / `EncodeString(s)` | `slang_string *slang_encoding_hex_encode(const slang_slice *data)` / `_encode_string` | lowercase |
| `hex.Decode(s) []byte!` / `DecodeString(s) string!` | `void slang_encoding_hex_decode(const slang_string *s, slang_slice *out, slang_error **err)` / `slang_string *slang_encoding_hex_decode_string(...)` | either case; errors `encoding/hex: invalid byte at offset N`, `encoding/hex: odd length hex string` |

**std.uuid** (`src/std_uuid.c`)

| Slang | C signature | Semantics / notes |
|---|---|---|
| `New() string` | `slang_string *slang_uuid_new(void)` | random version-4 UUID (RFC 9562) from `crypto.rand`, lowercase `8-4-4-4-12`; panics if no entropy source works |
| `IsValid(s) bool` | `bool slang_uuid_is_valid(const slang_string *s)` | canonical 36-char form, hex digits of either case, any version |

---

## 15. Platform layer

Everything OS-specific that the runtime needs is behind the internal interface
`src/platform/platform.h` (hidden `slang__plat_*` symbols, not part of the ABI);
see `docs/PLATFORMS.md`.

| File | Contents |
|---|---|
| `platform/posix.c` | threads (`slang__plat_thread_start[_detached]`, `_join`), `cpu_count`, `yield`, monotonic clock `mono_ns`, entropy `random_bytes` (`getrandom`/`getentropy`, then `/dev/urandom`), `backtrace` (execinfo) |
| `platform/poll_epoll.c` | the readiness poller on Linux: level-triggered epoll plus an eventfd for wake-ups (`poller_new/free/set/remove/wait/wake`) |
| `platform/poll_kqueue.c` | the poller for macOS/BSD: not implemented, the file stops the build there with `#error` |
| `platform/net_posix.c` | non-blocking TCP/UDP sockets (`tcp_connect`, `tcp_connect_result`, `tcp_listen`, `tcp_accept`, `udp_bind`, `sock_recv/send`, `udp_recvfrom/sendto`, `sock_close`, `sock_addr`, `sock_port`), `is_ip`, `resolve` (getaddrinfo), `strerror`. Results are `>= 0` or `-errno`; "would block" is `-SLANG_PLAT_EAGAIN` |

Users of the layer: `task.c` (worker and timer threads, CPU count), `task_io.c`
(poller), `panic.c` (backtrace), `std_crypto.c`/`std_random.c` (entropy),
`std_net.c` (sockets). Mutexes and condition variables are still pthreads used
directly in `task.c`; `std_time.c`/`std_fs.c`/`std_os.c` still call POSIX
directly (the Windows port moves them).

## 16. Networking: std.net and std.net.http

`std.net` and `std.net.http` are Slang (`compiler/src/sema/stdlib_src/net.sl`,
`http.sl`, see `compiler/src/sema/stdsrc.rs`) over the internal externs below
(`src/std_net.c`, platform-independent; Slang names start with `rt`). Every
socket is non-blocking; an operation that would block reports it and the Slang
code awaits `slang_net_rt_wait_readable/writable` (10.9), so no worker thread
ever blocks on the network.

| Slang (internal) | C signature | Semantics |
|---|---|---|
| `rtConnect(ip, port) int!` | `int64_t slang_net_rt_connect(const slang_string *ip, int64_t port, slang_error **err)` | new TCP socket, connect started; the caller awaits writability, then `rtConnectResult` |
| `rtConnectResult(fd) void!` | `void slang_net_rt_connect_result(int64_t fd, slang_error **err)` | `SO_ERROR` of the connect (`connect: Connection refused`) |
| `rtListen(ip, port) int!` | `int64_t slang_net_rt_listen(const slang_string *ip, int64_t port, slang_error **err)` | bound, listening socket (`SO_REUSEADDR`, backlog 128); `ip` must be numeric |
| `rtAccept(fd) int!` | `int64_t slang_net_rt_accept(int64_t fd, slang_error **err)` | new connection fd, or **-1** if none is pending |
| `rtRead(fd, n) []byte!` | `void slang_net_rt_read(int64_t fd, int64_t n, slang_slice *out, slang_error **err)` | up to `n` bytes; empty at end of stream **or** when it would block: `rtAgain()` tells which |
| `rtAgain() bool` | `bool slang_net_rt_again(void)` | the last `rtRead`/`rtRecvFrom` on this thread would have blocked (read it right after the call) |
| `rtWrite(fd, data, off) int!` / `rtWriteStr(fd, s, off) int!` | `int64_t slang_net_rt_write(int64_t fd, const slang_slice *data, int64_t off, slang_error **err)` / `_write_str(..., const slang_string *s, ...)` | writes from byte `off`; bytes written or **-1** if it would block (`MSG_NOSIGNAL`: no SIGPIPE) |
| `rtClose(fd)` | `void slang_net_rt_close(int64_t fd)` | `slang_io_forget` + close |
| `rtWaitReadable(fd, ms)` / `rtWaitWritable` `Task<int>` | `slang_task *slang_net_rt_wait_readable(int64_t fd, int64_t ms)` | `slang_io_wait_*` with `ms <= 0` meaning no timeout |
| `rtLocalAddr(fd)` / `rtRemoteAddr(fd) string`, `rtPort(fd) int` | `slang_string *slang_net_rt_local_addr(int64_t)`, ... | `"ip:port"` (`"[ip]:port"` for IPv6), local port |
| `rtIsIP(s) bool` | `bool slang_net_rt_is_ip(const slang_string *s)` | numeric IPv4/IPv6 |
| `rtResolve(host) Task<int>` / `rtResolveTake(h) []string!` | `slang_task *slang_net_rt_resolve(const slang_string *host)` / `void slang_net_rt_resolve_take(int64_t h, slang_slice *out, slang_error **err)` | getaddrinfo on a detached helper thread; the task's result is a request handle that `rtResolveTake` turns into the addresses (IPv4 first, no duplicates) or `lookup host: reason`, freeing it. IP literals complete at once |
| `rtUdpBind(ip, port) int!`, `rtRecvFrom(fd, n) []byte!`, `rtLastFrom() string`, `rtLastFromPort() int`, `rtSendTo(fd, data, ip, port) int!` | `slang_net_rt_udp_bind`, `_recv_from`, `_last_from`, `_last_from_port`, `_send_to` | UDP; the sender of the last datagram is kept per thread like `rtAgain` |

**Public Slang API** (the authoritative list with semantics is DESIGN.md, "Сеть и
протоколы"): `net.Dial(addr) Task<Conn!>`, `net.DialTimeout(addr, ms)`,
`net.Listen(addr) Listener!`, `net.ListenUDP(addr) UDPConn!`,
`net.ResolveIP(host) Task<[]string!>`, `net.SplitHostPort`, `net.JoinHostPort`;
`Conn` (`Read(n)`, `ReadString(n)`, `Write(b)`, `WriteString(s)`, `SetTimeout(ms)`,
`Close()`, `IsClosed()`, `LocalAddr()`, `RemoteAddr()`), `Listener` (`Accept()`,
`Close()`, `Addr()`, `Port()`), `UDPConn` (`RecvFrom(n) Task<Packet!>`,
`SendTo(b, addr)`, ...). `Conn`, `Listener` and `UDPConn` are handles to a heap
socket object whose `Drop` closes the descriptor, so a socket is closed by
`Close()` or when the last copy goes away (and `--leak-check` sees forgotten
ones).

**HTTP** (`http.sl`): parsing and formatting are Slang, over a buffered reader
on `net.Conn`; bodies are strings. Client: `http.Get(url)`, `http.Post(url, body,
contentType)` and `http.Client{Timeout, MaxRedirects, Headers}` with
`Get/Post/Do`, returning `Task<Response!>`; HTTP/1.1 with `Connection: close`
(no pooling), `Content-Length`, chunked and read-to-end bodies, redirects
301/302/303/307/308 (at most 10; 303 and a redirected POST become GET); `https`
fails with `https is not supported yet`. Server: `http.Server{Addr, Handler,
IdleTimeout}` with `Listen()`, `Port()`, `Serve() Task<void!>`, `Shutdown()
Task<void>`; one task per connection, HTTP/1.1 keep-alive, request bodies with
`Content-Length` or chunked, handler errors become 500. `interface Handler {
func Serve(req *Request, res *Response) Task<void!> }` (an interface method
cannot be `async`, so it returns the task of an async call); `http.NewRouter()`
implements it with `Get/Post/Put/Delete/Handle(pattern, handler)`, `{name}`
parameters and a final `*`.

---

## 17. Reflection tables and std.encoding.json

Emitted by the compiler (`compiler/src/codegen/reflect.rs`) only for programs
that use `std.reflect` (directly, through `typeof`, `json`, or a builtin
attribute named as a type). C declarations and layout checks
(`_Static_assert`) are in the reflection section of `slang_rt.h` and in
`src/std_reflect.c`.

### 17.1 Type tables

With reflection on, **every `slang_type_info` the program emits is the head
of a full table**: the type id stored in box headers, at `vtable[0]`, in the
second word of an `any` and in `slang_error.user_type` is a
`slang_reflect_type *`. Tables are immortal objects
`{ i64 -1, ptr NULL, slang_reflect_type }`; the type id is the address of the
payload (object + 16), so a Slang `*reflect.TypeInfo` and the type id are the
same pointer and `==` on them compares types (one table per type: tables are
deduplicated by type in the compiler). Only types that some code or table
refers to get a table; unreferenced ones are removed by `globaldce`.

```c
struct slang_reflect_type {             /* std.reflect.TypeInfo, 232 bytes */
    slang_type_info rt;                 /* +0   name (C string), 16 + sizeof T, drop */
    int32_t kind;                       /* +24  slang_reflect_kind (std.reflect.Kind order) */
    slang_string *name;                 /* +32  short name: User, *User, []int, map[string]int */
    slang_string *full_name;            /* +40  namespace path + name */
    int64_t size, align;                /* +48, +56 */
    slang_slice fields;                 /* +64  []*FieldInfo of a struct */
    slang_slice methods;                /* +96  []*MethodInfo (full types only) */
    slang_slice variants;               /* +128 []*VariantInfo of an enum */
    const slang_reflect_type *elem;     /* +160 element of *T, weak *T, []T, [N]T, T!, Task<T>; map value */
    const slang_reflect_type *key;      /* +168 map key */
    int64_t len;                        /* +176 array length */
    slang_slice attrs;                  /* +184 []any, see 17.2 */
    const slang_reflect_type *ptr_to;   /* +216 table of *T (full types only, for TypeInfo.New) */
    const slang_reflect_type *canon;    /* +224 alias tables only */
};
```

`slang_reflect_field` (FieldInfo, 72 bytes: `name`, `type`, `offset`,
`index`, `embedded`, `exported`, `attrs` at +40), `slang_reflect_method`
(MethodInfo, 72 bytes: `name`, `type`, `is_static` at +16, `invoke` at +24,
`fnval` at +32, `attrs` at +40) and `slang_reflect_variant` (VariantInfo, 80
bytes: `name`, `index`, `fields` at +16, `attrs` at +48) are immortal objects
too; the slices holding them (and every other table slice) have a NULL
backing, so retaining or releasing them is a no-op. Strings are immortal
literals.

- Field offsets are the C layout of the struct (declaration order, natural
  alignment). Variant fields have offsets from the start of the enum value
  (`{i32 tag, payload}`, payload at +8).
- *Full* types — named in `reflect.TypeOf<T>()`, in a static `typeof(x)`, in
  `reflect.Fields<T>()`, or declared `@reflect` — list their non-async
  methods and get `ptr_to`; other tables have no methods.
- A struct field spelled with a builtin alias (`int64`, `uint`, `uint8`,
  `float64`) points at an *alias table*: a copy with that name whose `canon`
  is the real table (`Value.Get/Set` compare canonical tables).

### 17.2 Attribute values

Every attribute is a constant value of its attribute struct (`@json` is
`std.reflect.json {name string, omitEmpty bool, skip bool}`, `@deprecated`
is `{message string}`, user attributes are their `@attribute` structs, with
field defaults filled in). `attrs` is a `[]any` in source order whose
elements hold a `*A`: `{data, type}` with `type` = the table of `*A` and
`data` = an immortal box whose payload is the pointer to the immortal value
object. `@attribute` and `@reflect` themselves are not listed.

### 17.3 Methods

`invoke` is `{any, slang_error*} invoke(any recv, slang_slice args)` (Slang
ABI: aggregates by value): it accepts a receiver boxed as `*S` or `S`, checks
the argument count and that every argument's dynamic type is the parameter
type (`any` parameters take the value as is), unboxes them (retaining values
for moved parameters), calls the method and boxes a non-void result into a
new `any` (+1); a method error (or a check failure, `reflect: S.M: ...`) is
returned as the error. `fnval` is a closure-convention thunk
`fn(env, recv, args...)` (`MethodInfo.Call<F>()` returns `{fnval, NULL}`).

### 17.4 std.encoding.json

| Slang | C | Notes |
|---|---|---|
| `json.Quote(s)` | `slang_string *slang_encoding_json_quote(const slang_string *s)` | JSON string literal: quotes, `\" \\ \n \r \t \b \f`, other control bytes as `\u00XX`; other bytes (UTF-8) unchanged |
| internal `json.scan(text)` | `void slang_encoding_json_scan(const slang_string *text, slang_slice *out, slang_error **err)` | `[]string` of tokens (`elem_size` 8, `slang_ops_ref`): `{ } [ ] : ,`, `s` + unescaped string bytes (`\uXXXX` and surrogate pairs to UTF-8, lone surrogates U+FFFD), `n` + number text (RFC 8259 syntax), `t` `f` `z` (true, false, null). Lexical errors: `json: <what> at offset N` (byte offset) |

The parser, `json.Value`, `Parse<T>` and `Format<T>` are Slang
(`compiler/src/sema/reflect_src/json.sl`).
