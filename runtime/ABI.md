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

---

## 1. Building and linking

```
make            # -> runtime/libslang_rt.a (also runtime/build/libslang_rt.a)
make test       # C unit tests + a real llvm.coro example (tests/coro_lowering.ll)
make clean
```

Link a Slang program with:

```
clang prog.o runtime/libslang_rt.a -lpthread -rdynamic -o prog
```

`-rdynamic` is optional. It only makes function names appear in panic backtraces.

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

A pointer to a heap object always points at its header (offset 0), never at the
payload. So for `new Point{X: 1, Y: 2}`, `*Point` is a pointer to
`{ %slang_object, double, double }`, and field `X` is at offset 16.

### 3.2 Functions

| Function | Semantics |
|---|---|
| `void *slang_alloc(int64_t size)` | `calloc`-style **zeroed** raw memory, not refcounted. Use it for coroutine frames, closure environments without a header, and scratch memory. Out of memory or a negative size panics. `size == 0` returns a unique pointer |
| `void *slang_realloc(void *p, int64_t size)` | raw `realloc`. New bytes are **not** zeroed. Panics on out of memory |
| `void slang_free(void *p)` | frees raw memory. NULL is fine |
| `void *slang_object_new(const slang_type_info *type, int64_t size)` | allocates a zeroed object of `size` bytes, **header included** (so `size >= 16`), sets `refcount = 1` and `type = type`. Returns it +1 |
| `void slang_retain(void *obj)` | atomic `++refcount`. No-op for NULL and for immortal objects |
| `void slang_release(void *obj)` | atomic `--refcount`. When it reaches 0: calls `type->drop(obj)` if both are non-NULL, then frees the memory. No-op for NULL and immortal objects |
| `int64_t slang_refcount(const void *obj)` | current count (debugging and tests). 0 for NULL |
| `int64_t slang_live_objects(void)` | number of objects allocated with `slang_object_new` and not yet freed (leak checks in tests) |

**The `drop` contract.** `drop(obj)` must release every reference the object holds:
strings, pointers, slices (`slang_slice_release`) and so on. It must **not** free
`obj`, because the runtime does that after `drop` returns. The compiler emits one
`drop` per heap-allocated struct type that has reference-typed fields. If there
are no such fields, `type` may point to a type info whose `drop` is NULL, or
`type` itself may be NULL.

**Immortal objects** (`refcount == -1`, `SLANG_RC_IMMORTAL`) are never modified or
freed. Use them for string literals and sentinel errors emitted as LLVM globals.

There is no cycle collector. `weak *T` from DESIGN.md is not implemented in 0.1.

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
- `&slang_ops_ref`: the slot is one 8-byte heap-object pointer (string, `*T`, map,
  error, Task...).
- `&slang_ops_slice`: the slot is one 32-byte `slang_slice`.
- For struct values containing references, the compiler emits a constant
  `slang_value_ops` whose functions retain or release each field.

`release` must handle an all-zero slot (all release functions treat NULL as a
no-op), because containers zero-initialise their storage.

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
| `int64_t slang_string_to_int(s, slang_error **err)` | base-10 with an optional sign. No surrounding whitespace is allowed. On success `*err = NULL`. On failure it returns 0 and sets `*err` to an owned error (`invalid integer: "..."` or `integer out of range: "..."`). `err` may be NULL |
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
| `slang_slice_append(slang_slice *s, elem_size, ops, const void *elem)` | `s = append(s, elem)` **in place**. The element is copied and retained. If `len < cap` it writes into the shared backing, like Go. Otherwise it allocates a new backing array (growth: at least 4, doubling below 1024 elements, then x1.5), copies and retains the elements, and releases the old backing. `elem` may point into `s` itself. For `t = append(s, x)` with `t != s`, first do `slang_slice_assign(&t, &s)`, then append to `t` |
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
| `slang_map *slang_map_new(const slang_map_desc *d, int64_t hint)` | new empty map, +1. `hint` pre-sizes the bucket array |
| `slang_map_new_str(val_size, val_ops)` | `map[string]V`. The key slot is `slang_string *` (with `slang_ops_ref`) |
| `slang_map_new_int(val_size, val_ops)` | `map[int]V`. The key slot is `int64_t` |
| `int64_t slang_map_len(m)` | number of entries (0 for nil) |
| `void *slang_map_get(m, key)` | **[borrowed]** pointer to the value slot, or NULL if absent. The pointer is valid until the next `set`, `get_or_insert` or `delete` on the map. `v, ok = m[k]`: `ok = p != NULL`, and `v` is `*p` (retain it) or the zero value |
| `void slang_map_set(m, key, val)` | `m[k] = v`. Inserts (retaining key and value) or overwrites (retains the new value, releases the old one). Panics `assignment to entry in nil map` if `m` is NULL |
| `void *slang_map_get_or_insert(m, key)` | **[borrowed]** value slot for `key`, inserting a zeroed value if absent. Use it for `m[k] += 1` and `m[k].f = x`. Writing to the slot follows the store rule in 2.3. Panics on nil |
| `bool slang_map_delete(m, key)` | removes the entry (releasing the key and value). Returns whether it existed. A nil map gives false |
| `slang_map_get_str / set_str / delete_str(m, slang_string *key, ...)` | fast paths for string-keyed maps. The key is passed directly, not by pointer |
| `slang_map_get_int / set_int / delete_int(m, int64_t key, ...)` | fast paths for int-keyed maps |

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
- The order is unspecified (it is bucket order, not randomised).
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

typedef struct slang_error {            // 56 bytes
    slang_object hdr;                   // +0   type = &slang_type_error
    slang_string *message;              // +16  own message, or the wrap context (owned)
    struct slang_error *inner;          // +24  wrapped error (owned) or NULL
    const slang_type_info *user_type;   // +32  type of a boxed user error, or NULL
    void *payload;                      // +40  boxed user value (a slang object, owned) or NULL
    slang_message_fn message_fn;        // +48  user Message() thunk or NULL
} slang_error;
```

Treat the fields as read-only and use the functions below. The layout is
documented so the compiler can emit **static sentinel errors** directly:

```llvm
; const ErrNotFound = error("not found")
@ErrNotFound = constant { i64, ptr, ptr, ptr, ptr, ptr, ptr }
    { i64 -1, ptr @slang_type_error, ptr @.str.not_found, ptr null, ptr null, ptr null, ptr null }
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
| `bool slang_error_is(const slang_error *err, const slang_error *target)` | `errors.Is`: walks `err`, `err->inner`, ... and returns true if any link **is the same object** (pointer identity) as `target`. Sentinels work because they are unique objects. `is(NULL, NULL)` is true and `is(x, NULL)` is false for non-NULL `x` |
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
  `slang_release(err)`, then return `w`.
- `try e catch err { ... }` binds `err` (owned) and releases it at the end of the
  catch scope. `try e else d` releases `err` and uses `d`.
- Runtime functions that can fail use `slang_error **err` out-parameters
  (`*err = NULL` on success, an owned error on failure), or they return
  `slang_error *` directly (NULL = ok).

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

| Function | Semantics |
|---|---|
| `void slang_runtime_init(int argc, char **argv)` | stores the arguments and starts the worker pool and the timer thread. Idempotent. Call it first, on the main thread |
| `void slang_runtime_shutdown(void)` | waits until the job queue is empty and no job is running, stops the workers and the timer thread (pending timers are dropped without firing), flushes stdio, and frees the argument slice. Call it from the main thread only (calling it from a worker deadlocks). After shutdown the scheduler restarts lazily if used again |
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

---

## 13. Known limits in v0.1

- Value alignment is limited to 8 bytes (slice data is 16-aligned; map values and
  task results are only 8-aligned). Do not store `long double` or 16-byte SIMD
  types in containers.
- There is no cycle collection and no `weak` pointers yet.
- Map iteration order is deterministic but unspecified. Mutating a map during
  iteration is only partly supported (6.3).
- `errors.Is` compares identity only (7.2).
- I/O is blocking. There is no async file or network I/O yet, and no `select` over
  channels.
- The scheduler uses a single global queue (no work stealing). Blocking awaits on
  workers start spare threads.
- Panics do not unwind: no `defer`, no cleanup.
- Strings are byte strings: indexing, slicing and `len` work on bytes. Rune helpers
  are `slang_string_rune_count`, `slang_string_decode_rune` and
  `slang_string_from_rune`.
