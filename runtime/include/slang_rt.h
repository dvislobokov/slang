/*
 * slang_rt.h - public ABI of the Slang runtime library (libslang_rt.a), v0.1
 *
 * This is the single public header. ABI.md is the normative description of
 * every symbol and layout declared here; keep the two in sync.
 *
 * General conventions (see ABI.md "Conventions"):
 *   - Target: 64-bit (LP64). Pointers and int64_t are 8 bytes.
 *   - Arguments that are heap objects are BORROWED: the runtime never
 *     consumes a caller's reference. If the runtime stores a value it retains
 *     it itself.
 *   - Heap objects returned by a function are OWNED (+1): the caller must
 *     release them, unless the function is documented as [borrowed].
 *   - NULL is accepted everywhere a heap object is accepted, with the
 *     documented meaning (NULL string == "", NULL error == no error, ...).
 *   - Value types larger than 8 bytes (slang_slice, iterators) are always
 *     passed by pointer and returned through an out-pointer, so the compiler
 *     never has to implement C struct-by-value ABI rules.
 */
#ifndef SLANG_RT_H
#define SLANG_RT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SLANG_NORETURN __attribute__((noreturn))
#else
#define SLANG_NORETURN _Noreturn
#endif

#define SLANG_RT_VERSION_MAJOR 0
#define SLANG_RT_VERSION_MINOR 1

/* ======================================================================
 * Memory: object header, type info, value ops
 * ====================================================================== */

/* Refcount value marking a static/immortal object (literals, sentinels).
 * retain/release are no-ops on such objects. */
#define SLANG_RC_IMMORTAL ((int64_t)-1)
/* Refcount value of an object allocated in an arena (std.arena, ABI.md
 * 3.6): retain/release are no-ops, the arena frees it. */
#define SLANG_RC_ARENA ((int64_t)-2)
/* Bit 62 of a (non-immortal) refcount word: the object has a weak box.
 * Bits 57..61 belong to the cycle collector (ABI.md 3.8): CYCLIC marks an
 * object of a potentially cyclic type (set at allocation while the
 * collector is on; such objects always take the runtime's retain/release
 * paths), BUFFERED one recorded as a possible cycle root, MARKED one in the
 * set a running collection examines, DIRTY one whose count changed while
 * MARKED, ZOMBIE one whose count reached zero while BUFFERED (dropped; its
 * memory is freed by the buffer's owner). The count itself is
 * refcount & SLANG_RC_COUNT_MASK (the low 57 bits). */
#define SLANG_RC_WEAK_FLAG ((int64_t)1 << 62)
#define SLANG_RC_CYCLIC ((int64_t)1 << 61)
#define SLANG_RC_BUFFERED ((int64_t)1 << 60)
#define SLANG_RC_MARKED ((int64_t)1 << 59)
#define SLANG_RC_DIRTY ((int64_t)1 << 58)
#define SLANG_RC_ZOMBIE ((int64_t)1 << 57)
#define SLANG_RC_COUNT_MASK (SLANG_RC_ZOMBIE - 1)
/* Biased reference counting (ABI.md 3.1): an object without CYCLIC whose
 * word has BIASED is owned by one thread. Its word is then
 *   bits 0..15  B: the owner's count (plain 16-bit loads/stores by the owner)
 *   bits 16..46 S: the shared count, signed, stored + SLANG_RC_S_EXCESS
 *               (atomic 64-bit RMW of +-1 << 16 by other threads)
 *   bits 47..58 the owner's thread slot (1..4095)
 *   bit 59      QUEUED: S went negative; the object waits in its owner's queue
 *   bit 60      BIASED, bit 62 WEAK_FLAG
 * and the count is B + S. A word without BIASED (and without CYCLIC) is an
 * unowned count in the low 57 bits, as above. Bits 47..60 have these
 * meanings only in BIASED words; CYCLIC words keep the cycle collector's. */
#define SLANG_RC_BIASED ((int64_t)1 << 60)
#define SLANG_RC_QUEUED ((int64_t)1 << 59)
/* The same bit in an unowned (not BIASED, not CYCLIC) word: never bias this
 * object (the runtime sets it on objects that are shared between threads
 * by design, such as tasks). Not part of the count. */
#define SLANG_RC_NOBIAS SLANG_RC_QUEUED
#define SLANG_RC_B_MASK ((int64_t)0xFFFF)
#define SLANG_RC_S_SHIFT 16
#define SLANG_RC_S_FIELD ((int64_t)0x7FFFFFFF)     /* after >> SLANG_RC_S_SHIFT */
#define SLANG_RC_S_EXCESS ((int64_t)1 << 30)
#define SLANG_RC_OWNER_SHIFT 47
#define SLANG_RC_OWNER_SLOTS 4096
/* ((uint64_t)word >> SLANG_RC_OWNER_SHIFT) & SLANG_RC_OWNER_MASK equals the
 * calling thread's slang_rc_owner exactly when that thread owns the
 * (biased, non-cyclic, non-special) object. */
#define SLANG_RC_OWNER_MASK ((int64_t)0x16FFF)
#define SLANG_RC_NO_OWNER ((int64_t)0x1FFFF)          /* never matches */
/* Exit status of a program whose leak check found live objects. */
#define SLANG_LEAK_EXIT_CODE 66

typedef struct slang_type_info {
    const char *name;          /* +0  human-readable type name (may be NULL) */
    int64_t size;              /* +8  informational object size, 0 if unknown */
    void (*drop)(void *obj);   /* +16 releases children; must NOT free obj. May be NULL. */
} slang_type_info;             /* 24 bytes */

typedef struct slang_object {
    int64_t refcount;              /* +0 accessed atomically; SLANG_RC_IMMORTAL = static */
    const slang_type_info *type;   /* +8 may be NULL (no drop) */
} slang_object;                    /* 16 bytes; payload starts at +16 */

/* Ownership hooks for a value stored inline in a container slot
 * (slice element, map key/value, task result, channel element).
 * NULL ops pointer == plain data, nothing to retain/release. */
/* Cycle-collector visitor callback (ABI.md 3.8): `obj` is the header of a
 * referenced object; `tid` is NULL when the object is statically of a
 * cyclic type, else a dynamic type id (interface/any type word, closure
 * function pointer) the collector checks against its registry first. */
typedef void (*slang_cycle_cb)(void *ctx, void *obj, const void *tid);
typedef struct slang_value_ops {
    void (*retain)(void *slot);    /* +0 add the references held by *slot */
    void (*release)(void *slot);   /* +8 drop the references held by *slot */
    /* +16 cycle collector: cb(ctx, obj, tid) for each reference in *slot
     * that may lie on a cycle, or NULL. Containers created with ops whose
     * visit is non-NULL are flagged SLANG_RC_CYCLIC (ABI.md 3.8). */
    void (*visit)(void *slot, slang_cycle_cb cb, void *ctx);
} slang_value_ops;                 /* 24 bytes */

/* Slot holds exactly one heap-object pointer (string, *T, map, error, task...). */
extern const slang_value_ops slang_ops_ref;
/* Slot holds one `*T` pointer to an object payload (object + 16), or NULL. */
extern const slang_value_ops slang_ops_ptr;
/* Slot holds a slang_slice (32 bytes). */
extern const slang_value_ops slang_ops_slice;

void *slang_alloc(int64_t size);              /* zeroed raw memory, OOM -> panic */
void *slang_realloc(void *p, int64_t size);   /* raw; new bytes NOT zeroed */
void  slang_free(void *p);                    /* raw; NULL ok */

void *slang_object_new(const slang_type_info *type, int64_t size); /* rc=1, zeroed, size includes header */
void  slang_retain(void *obj);                /* NULL / immortal: no-op */
void  slang_release(void *obj);               /* NULL / immortal: no-op; at 0: drop + free */
/* 0 while the process has only one thread (refcounts may then be updated
 * without atomic RMW); set for good before the second thread starts. */
extern uint8_t slang_threaded;
/* The calling thread's owner tag for biased words: (BIASED >> 47) | slot,
 * or SLANG_RC_NO_OWNER for a thread without a slot (ABI.md 3.1). */
#ifdef __cplusplus
extern thread_local int64_t slang_rc_owner;
#else
extern _Thread_local int64_t slang_rc_owner;
#endif
int64_t slang_refcount(const void *obj);      /* debugging / tests; 0 for NULL */
int64_t slang_live_objects(void);             /* number of live slang_object_new objects */

/* Cycle collector (ABI.md 3.8). A registry entry describes objects whose
 * type info is `key` (visit/fin/clear take the object header; fin runs the
 * destructors only, clear releases the references without them; NULL
 * clear means type->drop), or, with visit == NULL, is a dynamic type id
 * (key only) whose referenced objects may be flagged. `alias` is a
 * dynamic type id (ti.<*S> of a type S, a closure's function pointer for
 * its environment) registered with the entry, or NULL; `state` is the
 * runtime's (0 until slang_cycles_new_entry registered it). */
typedef struct slang_cycle_type {
    const void *key;                                        /* +0  */
    void (*visit)(void *obj, slang_cycle_cb cb, void *ctx); /* +8  */
    void (*fin)(void *obj);                                 /* +16 */
    void (*clear)(void *obj);                               /* +24 */
    const void *alias;                                      /* +32 */
    int64_t state;                                          /* +40 */
} slang_cycle_type;                                         /* 48 bytes */
typedef struct slang_cycle_stats {
    int64_t runs;          /* collections run (dry runs included) */
    int64_t collected;     /* objects freed by the collector so far */
    int64_t candidates;    /* possible roots recorded so far */
    int64_t examined;      /* objects examined by all collections */
    int64_t pending;       /* published possible roots waiting for a collection */
    int64_t threshold;     /* auto mode: published roots that trigger one */
    int64_t aborted;       /* collections abandoned (set over SLANG_CYCLES_MAX_SCAN) */
    int64_t mode;          /* 0 off, 1 manual, 2 auto */
} slang_cycle_stats;
#define SLANG_CYCLES_OFF 0
#define SLANG_CYCLES_MANUAL 1
#define SLANG_CYCLES_AUTO 2
/* Registers `n` entries (kept by pointer; call before objects of those types exist). */
void slang_cycles_register(const slang_cycle_type *types, int64_t n);
/* slang_object_new for a cyclic type: sets SLANG_RC_CYCLIC unless the
 * collector is off; may run a pending automatic collection first. */
void *slang_cycles_new(const slang_type_info *type, int64_t size);
/* The same for the entry's type (e->key), registering the entry (and its
 * alias) first if needed: what compiled code calls. */
void *slang_cycles_new_entry(slang_cycle_type *e, int64_t size);
/* The same for a dynamic type: flagged when `type` is a registered object type. */
void *slang_cycles_new_dyn(const slang_type_info *type, int64_t size);
/* Runs a collection now on the calling thread; returns the objects freed. */
int64_t slang_cycles_collect(void);
void slang_cycles_stats(slang_cycle_stats *out);
int32_t slang_cycles_mode(void);
/* SLANG_CYCLES_MANUAL <-> SLANG_CYCLES_AUTO (tests); OFF only before the first flagged object. */
void slang_cycles_set_mode(int32_t mode);
/* std.runtime.CollectCycles / CycleStats (stdlib_src/runtime_cycles.sl):
 * stat 0 runs, 1 collected, 2 candidates, 3 examined, 4 pending,
 * 5 threshold, 6 aborted, 7 mode. */
int64_t slang_runtime_rt_collect_cycles(void);
/* The collection epoch and the release paired with a pin (ABI.md 3.8). */
extern int64_t slang_cycles_epoch;
void slang_ptr_release_noroot(void *payload, int64_t epoch);
/* std.diag.HeapByType (diag_heap.c): "live\ttotal\tbytes\ttype" lines. */
struct slang_string *slang_diag_rt_heap_types(void);
bool slang_diag_rt_heap_counting(void);
int64_t slang_runtime_rt_cycle_stat(int64_t which);

/* Leak check (ABI.md 3.5): pointer fields of a struct type, so the report can
 * name the fields that close a cycle. `fields` ends with offset -1. */
typedef struct slang_leak_field {
    int64_t offset;            /* +0 byte offset of a `*T` field in the payload */
    const char *name;          /* +8 "Type.Field" */
} slang_leak_field;
/* Leak check mode 2: `type` is @acyclic (`name` its short name); a leaked
 * object of it on a cycle is reported (ABI.md 3.5). */
void slang_leak_acyclic(const slang_type_info *type, const char *name);
void slang_leak_fields(const slang_type_info *type, const slang_leak_field *fields);

/* Arenas (std.arena, ABI.md 3.6). Handles are raw pointers passed as int64. */
int64_t slang_arena_create(void);
void slang_arena_destroy(int64_t arena);       /* frees everything and the arena */
void slang_arena_free(int64_t arena);          /* frees everything; the arena stays usable */
void *slang_arena_object(int64_t arena, const slang_type_info *type, int64_t size); /* zeroed, rc SLANG_RC_ARENA */
struct slang_slice;
struct slang_string;
void slang_arena_slice(struct slang_slice *out, int64_t arena, int64_t elem_size, int64_t n,
                       const slang_value_ops *ops);
struct slang_string *slang_arena_string(int64_t arena, const struct slang_string *s); /* borrowed in, arena-owned out */
int64_t slang_arena_objects(int64_t arena);    /* objects currently allocated in the arena */
void  slang_ptr_retain(void *payload);        /* retain the object whose payload (obj+16) is given; NULL ok */
void  slang_ptr_release(void *payload);       /* release likewise; NULL ok */
/* Leak check (`slang run --leak-check`): call before anything else. Mode 1
 * counts, mode 2 also records type names. slang_runtime_shutdown then prints
 * "leak: N objects" (+ "  K x Type" lines) to stderr and exits with
 * SLANG_LEAK_EXIT_CODE if objects are still alive. */
void  slang_leak_check_enable(int32_t mode);

/* Weak references: a weak box shared by all weak refs to one object. */
void *slang_weak_new(void *payload);          /* +1 weak box for the object of `payload`; NULL -> NULL */
void *slang_weak_get(void *box);              /* +1 payload pointer, or NULL if freed (or box NULL) */

/* ======================================================================
 * Strings: pointer to immutable heap object; NULL == ""
 * ====================================================================== */

typedef struct slang_string {
    slang_object hdr;   /* +0  */
    int64_t len;        /* +16 length in bytes */
    char *data;         /* +24 the bytes. data[len] is always readable and is
                         *     '\0' except in a view (ABI.md 4.1): use the
                         *     length, or slang_string_cstr() for C APIs */
} slang_string;         /* 32 bytes; an owned string's bytes follow at +32 */

extern const slang_type_info slang_type_string;
/* Type of a standalone view {hdr, len, data, owner @32}: its drop releases
 * the owner (ABI.md 4.1). */
extern const slang_type_info slang_type_string_view;

/* Refcount word of a view stored inside a view block (strings.Split...):
 * SLANG_RC_VIEW | address of the block; retain/release go to the block.
 * Every such word is < SLANG_RC_ARENA. */
#define SLANG_RC_VIEW ((int64_t)INT64_MIN)
static inline int slang_rc_is_view(int64_t rc) { return rc < SLANG_RC_ARENA; }

/* Static storage of an immortal string literal of N bytes (N includes the
 * NUL) and its initializer; `self` names the variable being defined. */
#define SLANG_STRING_STORAGE(N) struct { slang_object hdr; int64_t len; char *data; char bytes[N]; }
#define SLANG_STRING_INIT(self, lit) \
    { { SLANG_RC_IMMORTAL, &slang_type_string }, (int64_t)(sizeof(lit) - 1), (self).bytes, lit }

/* Static literal for C code. The compiler emits the equivalent LLVM global. */
#define SLANG_STATIC_STRING(name, lit)                                          \
    static SLANG_STRING_STORAGE(sizeof(lit)) name##_storage =                   \
        SLANG_STRING_INIT(name##_storage, lit);                                 \
    static slang_string *const name = (slang_string *)&name##_storage

slang_string *slang_string_new(const char *bytes, int64_t len);   /* owned copy */
slang_string *slang_string_from_cstr(const char *cstr);           /* owned copy */
slang_string *slang_string_literal(const char *bytes, int64_t len); /* immortal copy, never freed */
const char   *slang_string_data(const slang_string *s);  /* [borrowed] bytes (NOT NUL-terminated for a view), "" for NULL */
/* [borrowed-ish] NUL-terminated bytes for C APIs: s->data when it is
 * terminated, else a copy in a per-thread ring of 16 buffers, valid until
 * the 16th next call on this thread (use it within one C call). */
const char   *slang_string_cstr(const slang_string *s);
int64_t slang_string_len(const slang_string *s);         /* bytes */
int64_t slang_string_rune_count(const slang_string *s);  /* code points; invalid byte = 1 rune */
slang_string *slang_string_concat(const slang_string *a, const slang_string *b);
/* a + b, CONSUMING the caller's reference to `a` (result +1): when `a` is
 * uniquely referenced it is extended in place (amortised O(len(b))). */
slang_string *slang_string_append(slang_string *a, const slang_string *b);
int32_t slang_string_compare(const slang_string *a, const slang_string *b); /* -1, 0, 1 bytewise */
bool    slang_string_eq(const slang_string *a, const slang_string *b);
uint64_t slang_string_hash(const slang_string *s);        /* FNV-1a 64 of the bytes */
slang_string *slang_string_slice(const slang_string *s, int64_t lo, int64_t hi); /* bytes [lo,hi) */
uint8_t slang_string_byte_at(const slang_string *s, int64_t i);   /* bounds-checked */
int32_t slang_string_decode_rune(const slang_string *s, int64_t i, int64_t *width); /* U+FFFD on error */
int64_t slang_string_index(const slang_string *s, const slang_string *sub); /* byte index or -1 */
bool    slang_string_contains(const slang_string *s, const slang_string *sub);
bool    slang_string_has_prefix(const slang_string *s, const slang_string *prefix);
bool    slang_string_has_suffix(const slang_string *s, const slang_string *suffix);

slang_string *slang_string_from_int(int64_t v);
slang_string *slang_string_from_uint(uint64_t v);
slang_string *slang_string_from_float(double v);
slang_string *slang_string_from_bool(bool v);
slang_string *slang_string_from_rune(int32_t r);

struct slang_error;
int64_t slang_string_to_int(const slang_string *s, struct slang_error **err);
double  slang_string_to_float(const slang_string *s, struct slang_error **err);

/* ======================================================================
 * Slices: 32-byte value, always handled through pointers
 * ====================================================================== */

typedef struct slang_slice {
    void *data;            /* +0  first element (NULL when backing is NULL) */
    int64_t len;           /* +8  */
    int64_t cap;           /* +16 elements available from data */
    slang_object *backing; /* +24 owned reference to the backing array, or NULL */
} slang_slice;             /* 32 bytes; all-zero == nil/empty slice */

void slang_slice_new(slang_slice *out, int64_t elem_size, int64_t len, int64_t cap);
void slang_slice_new_ops(slang_slice *out, int64_t elem_size, int64_t len, int64_t cap,
                         const slang_value_ops *ops);
void slang_slice_from(slang_slice *out, int64_t elem_size, const slang_value_ops *ops,
                      const void *elems, int64_t n);
void slang_slice_retain(const slang_slice *s);
void slang_slice_release(slang_slice *s);           /* also zeroes *s */
void slang_slice_assign(slang_slice *dst, const slang_slice *src); /* retain src, release old dst */
void *slang_slice_index(const slang_slice *s, int64_t i, int64_t elem_size); /* bounds-checked */
void slang_slice_append(slang_slice *s, int64_t elem_size, const slang_value_ops *ops,
                        const void *elem);
void slang_slice_append_slice(slang_slice *s, int64_t elem_size, const slang_value_ops *ops,
                              const slang_slice *other);
void slang_slice_sub(slang_slice *out, const slang_slice *s, int64_t lo, int64_t hi,
                     int64_t elem_size);
int64_t slang_slice_copy(slang_slice *dst, const slang_slice *src, int64_t elem_size,
                         const slang_value_ops *ops);
slang_string *slang_string_from_bytes(const slang_slice *bytes); /* []byte -> string */
void slang_string_to_bytes(slang_slice *out, const slang_string *s); /* string -> []byte */

/* ======================================================================
 * Maps: refcounted heap object; NULL == nil map
 * ====================================================================== */

typedef uint64_t (*slang_hash_fn)(const void *key);
typedef bool (*slang_eq_fn)(const void *a, const void *b);

typedef struct slang_map_desc {
    int64_t key_size;                 /* +0  */
    int64_t val_size;                 /* +8  */
    slang_hash_fn hash;               /* +16 */
    slang_eq_fn eq;                   /* +24 */
    const slang_value_ops *key_ops;   /* +32 may be NULL */
    const slang_value_ops *val_ops;   /* +40 may be NULL */
} slang_map_desc;                     /* 48 bytes; copied by slang_map_new */

typedef struct slang_map slang_map;   /* opaque */

typedef struct slang_map_iter {
    slang_map *map;    /* +0  borrowed */
    int64_t bucket;    /* +8  */
    void *next;        /* +16 */
    int64_t reserved;  /* +24 */
} slang_map_iter;      /* 32 bytes, contents private */

uint64_t slang_hash_bytes(const void *p, int64_t n);
uint64_t slang_hash_int(const void *key);   /* key -> int64_t */
bool     slang_eq_int(const void *a, const void *b);
uint64_t slang_hash_str(const void *key);   /* key -> slang_string* */
bool     slang_eq_str(const void *a, const void *b);

slang_map *slang_map_new(const slang_map_desc *desc, int64_t hint);
slang_map *slang_map_new_str(int64_t val_size, const slang_value_ops *val_ops);
slang_map *slang_map_new_int(int64_t val_size, const slang_value_ops *val_ops);
int64_t slang_map_len(const slang_map *m);
void   *slang_map_get(const slang_map *m, const void *key);      /* [borrowed] value slot or NULL */
void    slang_map_set(slang_map *m, const void *key, const void *val);
void   *slang_map_get_or_insert(slang_map *m, const void *key);  /* zeroed slot if new */
bool    slang_map_delete(slang_map *m, const void *key);
void   *slang_map_get_str(const slang_map *m, const slang_string *key);
void    slang_map_set_str(slang_map *m, const slang_string *key, const void *val);
bool    slang_map_delete_str(slang_map *m, const slang_string *key);
void   *slang_map_get_or_insert_str(slang_map *m, const slang_string *key); /* zeroed slot if new */
void   *slang_map_get_int(const slang_map *m, int64_t key);
void    slang_map_set_int(slang_map *m, int64_t key, const void *val);
bool    slang_map_delete_int(slang_map *m, int64_t key);
void   *slang_map_get_or_insert_int(slang_map *m, int64_t key);            /* zeroed slot if new */
void    slang_map_iter_init(slang_map_iter *it, slang_map *m);
bool    slang_map_iter_next(slang_map_iter *it, void **key, void **val);

/* ======================================================================
 * Errors
 * ====================================================================== */

typedef slang_string *(*slang_message_fn)(void *payload); /* returns OWNED string */

/* Static record of a source location, emitted by the compiler in read-only
 * data (one per error-creation site, `?` and catching `try`). */
typedef struct slang_error_site {
    const char *file;                 /* +0  path as given to the compiler */
    const char *func;                 /* +8  function name, e.g. "Load", "Cache.Get" */
    int64_t line;                     /* +16 1-based */
} slang_error_site;                   /* 24 bytes */

/* One `returned via` entry of an error trace. */
typedef struct slang_error_frame {
    const slang_error_site *site;
    slang_string *ctx;                /* `?("ctx")` context (owned ref) or NULL */
} slang_error_frame;                  /* 16 bytes */

#define SLANG_ERROR_INLINE_FRAMES 8

typedef struct slang_error {
    slang_object hdr;                 /* +0  */
    slang_string *message;            /* +16 own message / wrap context (owned ref) */
    struct slang_error *inner;        /* +24 wrapped error (owned ref) or NULL */
    const slang_type_info *user_type; /* +32 type of boxed user error or NULL */
    void *payload;                    /* +40 boxed user value: slang object (owned ref) or NULL */
    slang_message_fn message_fn;      /* +48 user Message() thunk or NULL */
    /* --- error trace (ABI.md 7.4); all zero for an error without one --- */
    struct slang_error *origin;       /* +56 the error this is a copy-on-write copy of (owned ref) or NULL */
    const slang_error_site *created;  /* +64 creation site or NULL */
    int64_t nframes;                  /* +72 number of `returned via` frames */
    slang_error_frame frames[SLANG_ERROR_INLINE_FRAMES]; /* +80 the first 8 frames */
    slang_error_frame *overflow;      /* +208 frames 8.. (raw heap array) or NULL */
    int64_t overflow_cap;             /* +216 capacity of overflow */
} slang_error;                        /* 224 bytes */

extern const slang_type_info slang_type_error;
extern slang_error *const slang_err_eof;   /* sentinel returned by stdin reads at EOF */

slang_error *slang_error_new(const slang_string *msg);
slang_error *slang_error_new_cstr(const char *msg);
slang_error *slang_error_sentinel(const slang_string *msg);  /* immortal, for `const E = error(..)` */
slang_error *slang_error_new_typed(const slang_type_info *type, void *payload,
                                   slang_message_fn message_fn);
slang_error *slang_error_wrap(slang_error *inner, const slang_string *ctx); /* NULL inner -> NULL */
slang_error *slang_error_unwrap(const slang_error *err);   /* owned inner or NULL */
slang_string *slang_error_message(const slang_error *err); /* owned; full chain text */
bool  slang_error_is(const slang_error *err, const slang_error *target);
void *slang_error_as(const slang_error *err, const slang_type_info *type); /* [borrowed] payload */
/* Error traces (ABI.md 7.4). Both take an OWNED err and return it (or its
 * copy-on-write replacement) OWNED; NULL err -> NULL. */
slang_error *slang_error_created(slang_error *err, const slang_error_site *site);
slang_error *slang_error_returned(slang_error *err, const slang_error_site *site,
                                  const slang_string *ctx);
slang_string *slang_error_trace(const slang_error *err);              /* owned; "" if none */
void slang_error_frames(const slang_error *err, slang_slice *out);    /* owned []errors.Frame */
extern const slang_value_ops slang_ops_error_frame;  /* ops of a 32-byte errors.Frame */
/* The double nearest to the shortest decimal that round-trips as float32
 * `f`; printing it with the float64 rules gives Go's float32 output. */
double slang_float32_shortest(float f);
/* "<op> <path>: <strerror(e)>"; for ENOENT the error wraps the immortal
 * sentinel std.fs.ErrNotFound (slang_fs_err_not_found), so errors.Is works. */
slang_error *slang_error_errno(const char *op, const char *path, int e);
slang_error *slang_fs_err_not_found(void);           /* std.fs.ErrNotFound, immortal */
/* std.fmt.Sprintf: Go verbs over the compiler's per-argument lowering
 * (kind letters, int values, float values, println texts; std_fmt_sprintf.c). */
slang_string *slang_fmt_sprintf(const slang_string *format, const slang_string *kinds, const slang_slice *ints,
                                const slang_slice *floats, const slang_slice *texts);
/* `main() void!` failed: "error: <message>" and the trace on stderr. */
void slang_error_report(const slang_error *err);

/* ======================================================================
 * Panic
 * ====================================================================== */

SLANG_NORETURN void slang_panic(const char *msg);
SLANG_NORETURN void slang_panic_str(const slang_string *msg);
SLANG_NORETURN void slang_panic_error(const slang_error *err);
SLANG_NORETURN void slang_panic_nil(void);
SLANG_NORETURN void slang_panic_bounds(int64_t idx, int64_t len);
SLANG_NORETURN void slang_panic_slice_bounds(int64_t lo, int64_t hi, int64_t cap);

/* ======================================================================
 * I/O
 * ====================================================================== */

void slang_print_str(const slang_string *s);
void slang_print_cstr(const char *s);
void slang_print_int(int64_t v);
void slang_print_uint(uint64_t v);
void slang_print_float(double v);
void slang_print_bool(bool v);
void slang_print_rune(int32_t r);
void slang_print_space(void);
void slang_print_newline(void);
void slang_println_str(const slang_string *s);
void slang_println_cstr(const char *s);
void slang_println_int(int64_t v);
void slang_println_uint(uint64_t v);
void slang_println_float(double v);
void slang_println_bool(bool v);
void slang_println_rune(int32_t r);
void slang_eprint_str(const slang_string *s);   /* to stderr */
void slang_eprintln_str(const slang_string *s);
void slang_flush(void);

slang_string *slang_read_file(const slang_string *path, slang_error **err);
slang_error  *slang_write_file(const slang_string *path, const slang_string *data);
slang_error  *slang_write_file_bytes(const slang_string *path, const slang_slice *data);
slang_string *slang_read_line(slang_error **err);  /* without '\n'; EOF -> NULL + slang_err_eof */

/* ======================================================================
 * Tasks / scheduler
 * ====================================================================== */

typedef struct slang_task slang_task;     /* opaque refcounted object */
typedef struct slang_mutex slang_mutex;   /* opaque refcounted object */
typedef struct slang_chan slang_chan;     /* opaque refcounted object */

typedef void (*slang_resume_fn)(void *frame);
typedef void (*slang_task_fn)(slang_task *task, void *ctx);

void    slang_sched_post(slang_resume_fn fn, void *arg);  /* run fn(arg) on a worker */
int64_t slang_sched_workers(void);
bool    slang_sched_is_worker(void);
void    slang_coro_resume(void *frame);   /* llvm.coro.resume for switched-resume frames */

slang_task *slang_task_new(int64_t result_size, const slang_value_ops *result_ops);
slang_task *slang_task_spawn(slang_task_fn fn, void *ctx);
slang_task *slang_task_spawn_ex(slang_task_fn fn, void *ctx, int64_t result_size,
                                const slang_value_ops *result_ops);
void  slang_task_complete(slang_task *task, const void *result);
bool  slang_task_is_done(const slang_task *task);
bool  slang_task_add_continuation(slang_task *task, slang_resume_fn fn, void *frame);
void *slang_task_result(const slang_task *task);  /* [borrowed] slot; panics if pending */
void *slang_task_await(slang_task *task);         /* blocking; [borrowed] slot */
slang_task *slang_task_delay(int64_t ms);
slang_task *slang_task_all(slang_task *const *tasks, int64_t n);
slang_task *slang_task_any(slang_task *const *tasks, int64_t n); /* result: int64_t index */

slang_mutex *slang_mutex_new(void);
slang_task  *slang_mutex_lock_async(slang_mutex *m);  /* completes when acquired */
void slang_mutex_lock(slang_mutex *m);                /* blocking */
bool slang_mutex_try_lock(slang_mutex *m);
void slang_mutex_unlock(slang_mutex *m);

slang_chan  *slang_chan_new(int64_t elem_size, const slang_value_ops *ops, int64_t capacity);
slang_task  *slang_chan_send_async(slang_chan *ch, const void *elem);  /* void result */
slang_task  *slang_chan_recv_async(slang_chan *ch); /* result: {int64 ok; elem @ +8} */
void slang_chan_send(slang_chan *ch, const void *elem);          /* blocking */
bool slang_chan_recv(slang_chan *ch, void *out);                 /* blocking; out gets owned copy */
void slang_chan_close(slang_chan *ch);
int64_t slang_chan_len(const slang_chan *ch);

/* Completes `task` unless it is already done; returns whether it did. */
bool slang_task_try_complete(slang_task *task, const void *result);

/* Task context (ABI.md 10.8): an opaque object per running task, inherited
 * by continuations, spawned jobs and tasks started from it. */
slang_object *slang_task_get_ctx(void);          /* +1 or NULL */
void slang_task_set_ctx(slang_object *ctx);      /* borrowed; retained */

/* A task for callback-style producers; unlike slang_task_new it does not
 * become the current task (ABI.md 10.11). */
slang_task *slang_task_new_plain(int64_t result_size, const slang_value_ops *result_ops);

/* Cancellation (ABI.md 10.11). */
void slang_task_cancel(slang_task *task);               /* cascades to descendants */
bool slang_task_is_cancelled(const slang_task *task);   /* NULL -> false */
bool slang_task_cur_cancelled(void);                    /* the current task */
void slang_task_set_deadline(slang_task *task, int64_t ms);  /* cancel after ms */
slang_task *slang_task_current(void);                   /* +1 or NULL */
slang_task *slang_task_new_scope(void);                 /* cancellation node under the current task, +1 */
slang_task *slang_task_enter_scope(slang_task *scope);  /* returns the previous current (+1) */
void slang_task_leave_scope(slang_task *prev);          /* borrowed */
slang_error *slang_task_err_cancelled(void);            /* std.task.ErrCancelled, immortal */
int64_t slang_task_next_key(void);

/* std.task primitives used by the Slang-source parts of std.task. */
slang_task *slang_task_rt_signal(void);          /* pending Task<void>, +1; fired when the current task is cancelled */
slang_task *slang_task_rt_waker(void);           /* the same, never fired by cancellation */
void slang_task_rt_fire(slang_task *signal);     /* complete (idempotent), cancels a delay */
void slang_task_spin_lock(int64_t *slot);        /* 0 = free, 1 = held */
void slang_task_spin_unlock(int64_t *slot);
slang_task *slang_task_yield(void);              /* completes from a fresh job, +1 */

/* Readiness waits (task_io.c, ABI.md 10.9). Result: int64_t
 * 0 = ready, 1 = timed out, 2 = the fd was released with slang_io_forget,
 * 3 = the waiting task was cancelled. */
slang_task *slang_io_wait_readable(int64_t fd, int64_t timeout_ms);  /* +1; timeout < 0 = none */
slang_task *slang_io_wait_writable(int64_t fd, int64_t timeout_ms);
void slang_io_forget(int64_t fd);   /* call before close(fd): wakes waiters with 2 */
void slang_io_register(int64_t fd); /* optional: assigns fd to a poller shard now (sockets do at creation) */

/* ======================================================================
 * Entry / process
 * ====================================================================== */

void slang_runtime_init(int argc, char **argv);
void slang_runtime_shutdown(void);
/* Waits for all queued and running jobs and stops the scheduler (it restarts
 * lazily if used again); slang_runtime_shutdown does the same first. The
 * compiler calls it before releasing the program's globals. */
void slang_runtime_drain(void);
void slang_os_args(slang_slice *out);   /* owned []string */
SLANG_NORETURN void slang_exit(int32_t code);

/* ======================================================================
 * Standard library (std.*) - symbols named by compiler/src/sema/stdlib.rs.
 * Mapping: string -> slang_string* (NULL == ""), int -> int64_t,
 * float -> double, bool -> bool, []T param -> const slang_slice*,
 * []T result -> trailing slang_slice *out (owned), T! -> T returned plus a
 * trailing slang_error **err (NULL on success, owned error on failure),
 * void! -> void with a trailing slang_error **err. Arguments are borrowed,
 * returned objects are owned (+1). See ABI.md section 14.
 * ====================================================================== */

/* std.strings ([]string: elem_size 8, ops &slang_ops_ref) */
void          slang_strings_split(const slang_string *s, const slang_string *sep, slang_slice *out);
slang_string *slang_strings_join(const slang_slice *parts, const slang_string *sep);
slang_string *slang_strings_trim(const slang_string *s);   /* Go strings.TrimSpace */
bool          slang_strings_contains(const slang_string *s, const slang_string *substr);
slang_string *slang_strings_replace(const slang_string *s, const slang_string *old,
                                    const slang_string *new_);  /* all occurrences */
slang_string *slang_strings_to_upper(const slang_string *s);
slang_string *slang_strings_to_lower(const slang_string *s);
bool          slang_strings_has_prefix(const slang_string *s, const slang_string *prefix);
bool          slang_strings_has_suffix(const slang_string *s, const slang_string *suffix);
int64_t       slang_strings_index(const slang_string *s, const slang_string *substr);
slang_string *slang_strings_repeat(const slang_string *s, int64_t count); /* count < 0 panics */
int64_t       slang_strings_to_int(const slang_string *s, slang_error **err);
double        slang_strings_to_float(const slang_string *s, slang_error **err);

/* std.math */
double slang_math_sqrt(double x);
double slang_math_abs(double x);
double slang_math_pow(double x, double y);
double slang_math_floor(double x);
double slang_math_ceil(double x);
double slang_math_min(double x, double y);
double slang_math_max(double x, double y);
double slang_math_sin(double x);
double slang_math_cos(double x);
double slang_math_tan(double x);
double slang_math_log(double x);
double slang_math_exp(double x);

/* std.fmt: args are the already-formatted []string */
slang_string *slang_fmt_format(const slang_string *tmpl, const slang_slice *args);

/* std.io */
slang_string *slang_io_read_file(const slang_string *path, slang_error **err);
void          slang_io_write_file(const slang_string *path, const slang_string *data,
                                  slang_error **err);
slang_string *slang_io_read_line(slang_error **err);  /* EOF: NULL + slang_err_eof */

/* std.errors (New -> slang_error_new, As -> slang_error_as) */
slang_error *slang_errors_wrap(const slang_error *err, const slang_string *msg); /* NULL err -> NULL */
slang_error *slang_errors_unwrap(const slang_error *err);  /* owned cause or NULL */
bool         slang_errors_is(const slang_error *err, const slang_error *target);
slang_string *slang_errors_trace(const slang_error *err);             /* errors.Trace */
void         slang_errors_frames(const slang_error *err, slang_slice *out); /* errors.Frames */

/* std.task.Delay is slang_task_delay (declared above). */

/* std.strings (more; std_strings_ext.c). []char: elem_size 4 (int32_t code
 * points), []byte: elem_size 1, both with NULL ops. */
void          slang_strings_fields(const slang_string *s, slang_slice *out);
int64_t       slang_strings_count(const slang_string *s, const slang_string *substr);
int64_t       slang_strings_last_index(const slang_string *s, const slang_string *substr);
slang_string *slang_strings_trim_prefix(const slang_string *s, const slang_string *prefix);
slang_string *slang_strings_trim_suffix(const slang_string *s, const slang_string *suffix);
slang_string *slang_strings_trim_left(const slang_string *s, const slang_string *cutset);
slang_string *slang_strings_trim_right(const slang_string *s, const slang_string *cutset);
void          slang_strings_split_n(const slang_string *s, const slang_string *sep, int64_t n,
                                    slang_slice *out);
bool          slang_strings_equal_fold(const slang_string *a, const slang_string *b);
slang_string *slang_strings_reverse(const slang_string *s);   /* by code point */
slang_string *slang_strings_pad_left(const slang_string *s, int64_t width, const slang_string *pad);
slang_string *slang_strings_pad_right(const slang_string *s, int64_t width, const slang_string *pad);
void          slang_strings_runes(const slang_string *s, slang_slice *out);
slang_string *slang_strings_from_runes(const slang_slice *runes);
void          slang_strings_bytes(const slang_string *s, slang_slice *out);
slang_string *slang_strings_from_bytes(const slang_slice *bytes);
int64_t       slang_strings_compare(const slang_string *a, const slang_string *b);
slang_string *slang_strings_title(const slang_string *s);

/* std.math (more; std_math_ext.c) */
double  slang_math_round(double x);
double  slang_math_trunc(double x);
double  slang_math_mod(double x, double y);
double  slang_math_hypot(double x, double y);
double  slang_math_atan2(double y, double x);
double  slang_math_log2(double x);
double  slang_math_log10(double x);
bool    slang_math_is_nan(double x);
bool    slang_math_is_inf(double x, int64_t sign);
double  slang_math_inf(int64_t sign);
double  slang_math_nan(void);
int64_t slang_math_imin(int64_t a, int64_t b);
int64_t slang_math_imax(int64_t a, int64_t b);
int64_t slang_math_iabs(int64_t a);

/* std.io (more; std_io_ext.c) */
slang_string *slang_io_read_all(slang_error **err);    /* stdin to EOF */
void          slang_io_read_lines(const slang_string *path, slang_slice *out, slang_error **err);
void          slang_io_write_lines(const slang_string *path, const slang_slice *lines,
                                   slang_error **err);
void          slang_io_eprint(const slang_string *s);
void          slang_io_eprintln(const slang_string *s);

/* std.time (std_time.c): times are int64 Unix milliseconds, calendar in UTC */
int64_t       slang_time_now(void);
int64_t       slang_time_now_nanos(void);
void          slang_time_sleep(int64_t ms);
int64_t       slang_time_since(int64_t start_ms);
slang_string *slang_time_format(int64_t ms, const slang_string *layout);  /* strftime + %L */
int64_t       slang_time_parse(const slang_string *s, const slang_string *layout,
                               slang_error **err);                       /* strptime */
int64_t       slang_time_year(int64_t ms);
int64_t       slang_time_month(int64_t ms);    /* 1..12 */
int64_t       slang_time_day(int64_t ms);      /* 1..31 */
int64_t       slang_time_hour(int64_t ms);
int64_t       slang_time_minute(int64_t ms);
int64_t       slang_time_second(int64_t ms);
int64_t       slang_time_weekday(int64_t ms);  /* 0 = Sunday */

/* std.os (std_os.c; Args is slang_os_args above) */
slang_string *slang_os_getenv(const slang_string *name);   /* "" if unset */
void          slang_os_setenv(const slang_string *name, const slang_string *value,
                              slang_error **err);
SLANG_NORETURN void slang_os_exit(int64_t code);
slang_string *slang_os_hostname(slang_error **err);
int64_t       slang_os_pid(void);
slang_string *slang_os_cwd(slang_error **err);
void          slang_os_chdir(const slang_string *path, slang_error **err);

/* std.fs (std_fs.c) */
bool          slang_fs_exists(const slang_string *path);
bool          slang_fs_is_dir(const slang_string *path);
void          slang_fs_read_dir(const slang_string *path, slang_slice *out, slang_error **err);
void          slang_fs_mkdir(const slang_string *path, slang_error **err);
void          slang_fs_mkdir_all(const slang_string *path, slang_error **err);
void          slang_fs_remove(const slang_string *path, slang_error **err);
void          slang_fs_remove_all(const slang_string *path, slang_error **err);
void          slang_fs_rename(const slang_string *from, const slang_string *to, slang_error **err);
int64_t       slang_fs_size(const slang_string *path, slang_error **err);
int64_t       slang_fs_mod_time(const slang_string *path, slang_error **err);
void          slang_fs_copy(const slang_string *from, const slang_string *to, slang_error **err);
slang_string *slang_fs_join(const slang_string *a, const slang_string *b);
slang_string *slang_fs_base(const slang_string *path);
slang_string *slang_fs_dir(const slang_string *path);
slang_string *slang_fs_ext(const slang_string *path);
slang_string *slang_fs_abs(const slang_string *path, slang_error **err);
slang_string *slang_fs_temp_dir(void);
void          slang_fs_append_file(const slang_string *path, const slang_string *data,
                                   slang_error **err);

/* std.random (std_random.c): xoshiro256**, process-wide state with a mutex */
void    slang_random_seed(int64_t seed);
int64_t slang_random_int(void);              /* [0, 2^63) */
int64_t slang_random_int_n(int64_t n);       /* [0, n); n <= 0 panics */
double  slang_random_float(void);            /* [0, 1) */
void    slang_random_bytes(int64_t n, slang_slice *out);  /* n < 0 panics */
void    slang_random_shuffle(const slang_slice *s);       /* []int, in place */

/* std.crypto.hash / std.crypto.hmac / std.crypto.rand (std_crypto.c).
 * []byte results: elem_size 1, NULL ops. Hex output is lowercase. */
void          slang_crypto_hash_sha256(const slang_string *data, slang_slice *out);
slang_string *slang_crypto_hash_sha256_hex(const slang_string *data);
void          slang_crypto_hash_sha512(const slang_string *data, slang_slice *out);
slang_string *slang_crypto_hash_sha512_hex(const slang_string *data);
void          slang_crypto_hash_sha1(const slang_string *data, slang_slice *out);
slang_string *slang_crypto_hash_sha1_hex(const slang_string *data);
void          slang_crypto_hash_md5(const slang_string *data, slang_slice *out);
slang_string *slang_crypto_hash_md5_hex(const slang_string *data);
int64_t       slang_crypto_hash_crc32(const slang_string *data);   /* IEEE, 0..2^32-1 */
void          slang_crypto_hmac_sha256(const slang_string *key, const slang_string *data,
                                       slang_slice *out);
slang_string *slang_crypto_hmac_sha256_hex(const slang_string *key, const slang_string *data);
bool          slang_crypto_hmac_equal(const slang_slice *a, const slang_slice *b); /* constant time */
void          slang_crypto_rand_bytes(int64_t n, slang_slice *out, slang_error **err);
int64_t       slang_crypto_rand_int(slang_error **err);             /* [0, 2^63) */
slang_string *slang_crypto_rand_hex(int64_t n, slang_error **err);  /* n bytes -> 2n chars */

/* std.encoding.base64 / std.encoding.hex (std_encoding.c) */
slang_string *slang_encoding_base64_encode(const slang_slice *data);       /* padded */
slang_string *slang_encoding_base64_encode_string(const slang_string *s);
void          slang_encoding_base64_decode(const slang_string *s, slang_slice *out,
                                           slang_error **err);
slang_string *slang_encoding_base64_decode_string(const slang_string *s, slang_error **err);
slang_string *slang_encoding_base64_encode_url(const slang_slice *data);   /* unpadded */
void          slang_encoding_base64_decode_url(const slang_string *s, slang_slice *out,
                                               slang_error **err);
slang_string *slang_encoding_hex_encode(const slang_slice *data);
slang_string *slang_encoding_hex_encode_string(const slang_string *s);
void          slang_encoding_hex_decode(const slang_string *s, slang_slice *out, slang_error **err);
slang_string *slang_encoding_hex_decode_string(const slang_string *s, slang_error **err);

/* std.uuid (std_uuid.c) */
slang_string *slang_uuid_new(void);                  /* random v4, lowercase */
bool          slang_uuid_is_valid(const slang_string *s);

/* std.net internals (std_net.c, ABI.md 16): used by the Slang-source std.net.
 * Sockets are handles (int64_t, never 0 or -1) that own the descriptor;
 * -1 / empty + slang_net_rt_again() mean "would block". Errors are the
 * causes (std.net sentinels or the system text); net.sl adds the context. */
slang_error *slang_net_err_closed(void);       /* std.net.ErrClosed etc.: immortal sentinels */
slang_error *slang_net_err_timeout(void);
slang_error *slang_net_err_refused(void);
slang_error *slang_net_err_reset(void);
slang_error *slang_net_err_unreachable(void);
slang_error *slang_net_err_addr_in_use(void);
slang_error *slang_net_err_not_found(void);
slang_error *slang_net_err_eof(void);          /* = slang_err_eof */
slang_error *slang_net_rt_op_error(const slang_error *boxed, const slang_string *prefix, const slang_error *inner);
int64_t slang_net_rt_dial(const slang_string *ip, int64_t port, const slang_string *lip, int64_t lport, slang_error **err);
int64_t slang_net_rt_dial_unix(const slang_string *path, slang_error **err);
void    slang_net_rt_connect_result(int64_t h, slang_error **err);
int64_t slang_net_rt_listen(const slang_string *ip, int64_t port, int64_t backlog, int64_t flags, slang_error **err);
int64_t slang_net_rt_listen_unix(const slang_string *path, int64_t backlog, slang_error **err);
int64_t slang_net_rt_accept(int64_t h, slang_error **err);
void    slang_net_rt_read(int64_t h, int64_t n, slang_slice *out, slang_error **err);
int64_t slang_net_rt_read_into(int64_t h, const slang_slice *buf, int64_t off, slang_error **err);
bool    slang_net_rt_again(void);
int64_t slang_net_rt_write(int64_t h, const slang_slice *data, int64_t off, slang_error **err);
int64_t slang_net_rt_write_str(int64_t h, const slang_string *s, int64_t off, slang_error **err);
int64_t slang_net_rt_write_v(int64_t h, const slang_slice *bufs, int64_t off, slang_error **err);
void    slang_net_rt_shutdown(int64_t h, int64_t how, slang_error **err);
void    slang_net_rt_close(int64_t h);
void    slang_net_rt_free(int64_t h);
bool    slang_net_rt_is_closed(int64_t h);
int64_t slang_net_rt_fd(int64_t h);
slang_task *slang_net_rt_wait_readable(int64_t h, int64_t ms);
slang_task *slang_net_rt_wait_writable(int64_t h, int64_t ms);
slang_string *slang_net_rt_local_addr(int64_t h);
slang_string *slang_net_rt_remote_addr(int64_t h);
int64_t slang_net_rt_port(int64_t h);
void    slang_net_rt_set_opt(int64_t h, int64_t opt, int64_t value, slang_error **err);
int64_t slang_net_rt_get_opt(int64_t h, int64_t opt, slang_error **err);
void    slang_net_rt_mcast(int64_t h, bool join, const slang_string *group, const slang_string *ifname, slang_error **err);
int64_t slang_net_rt_open_file(const slang_string *path, slang_error **err);
int64_t slang_net_rt_file_size(int64_t fd);
void    slang_net_rt_close_file(int64_t fd);
int64_t slang_net_rt_send_file(int64_t h, int64_t ffd, int64_t off, int64_t n, slang_error **err);
void    slang_net_rt_parse_ip(const slang_string *s, slang_slice *out);
slang_string *slang_net_rt_format_ip(const slang_slice *b);
bool    slang_net_rt_is_ip(const slang_string *s);
int64_t slang_net_rt_if_index(const slang_string *name);
slang_string *slang_net_rt_if_name(int64_t index);
void    slang_net_rt_interfaces(slang_slice *out, slang_error **err);
int64_t slang_net_rt_lookup_port(const slang_string *network, const slang_string *service);
slang_task *slang_net_rt_resolve(const slang_string *host, int64_t flags);   /* result: handle */
slang_task *slang_net_rt_reverse(const slang_string *ip);
void    slang_net_rt_resolve_take(int64_t handle, slang_slice *out, slang_error **err);
void    slang_net_rt_resolve_sync(const slang_string *host, int64_t flags, slang_slice *out, slang_error **err);
int64_t slang_net_rt_udp(const slang_string *ip, int64_t port, int64_t flags, slang_error **err);
int64_t slang_net_rt_udp_dial(const slang_string *ip, int64_t port, const slang_string *lip, int64_t lport, slang_error **err);
void    slang_net_rt_recv_from(int64_t h, int64_t n, slang_slice *out, slang_error **err);
int64_t slang_net_rt_read_from(int64_t h, const slang_slice *buf, slang_error **err);
slang_string *slang_net_rt_last_from(void);
int64_t slang_net_rt_last_from_port(void);
bool    slang_net_rt_last_trunc(void);
int64_t slang_net_rt_send_to(int64_t h, const slang_slice *data, const slang_string *ip, int64_t port,
                             slang_error **err);
slang_task *slang_net_rt_wait_any_writable(const slang_slice *hs, int64_t ms);
int64_t slang_net_rt_connect_check(int64_t h, slang_error **err);
void    slang_net_rt_bytes(int64_t n, slang_slice *out);
int64_t slang_net_rt_copy_bytes(const slang_slice *dst, int64_t doff, const slang_slice *src, int64_t soff, int64_t n);
int64_t slang_net_rt_index_byte(const slang_slice *b, int64_t from, int64_t c);
void    slang_net_rt_join(const slang_slice *a, const slang_slice *b, int64_t bn, slang_slice *out);

/* ---- reflection tables (ABI.md section 17, std_reflect.c) ----------------
 * Only programs that use std.reflect have them; then every slang_type_info
 * a compiled program emits is the first part of a slang_reflect_type, and
 * every table object is an immortal heap object (header refcount -1), so
 * the payload pointers below are valid Slang pointers. Slices of tables
 * have a NULL backing. Layouts are std.reflect.TypeInfo etc. in field
 * order with natural alignment (checked by _Static_assert in std_reflect.c). */
typedef struct slang_reflect_type slang_reflect_type;

typedef enum slang_reflect_kind {
    SLANG_KIND_INVALID, SLANG_KIND_VOID, SLANG_KIND_BOOL, SLANG_KIND_INT, SLANG_KIND_INT8,
    SLANG_KIND_INT16, SLANG_KIND_INT32, SLANG_KIND_UINT, SLANG_KIND_UINT8, SLANG_KIND_UINT16,
    SLANG_KIND_UINT32, SLANG_KIND_FLOAT, SLANG_KIND_FLOAT32, SLANG_KIND_CHAR, SLANG_KIND_STRING,
    SLANG_KIND_ERROR, SLANG_KIND_ANY, SLANG_KIND_INTERFACE, SLANG_KIND_STRUCT, SLANG_KIND_ENUM,
    SLANG_KIND_POINTER, SLANG_KIND_WEAK, SLANG_KIND_SLICE, SLANG_KIND_ARRAY, SLANG_KIND_MAP,
    SLANG_KIND_TUPLE, SLANG_KIND_FUNC, SLANG_KIND_FALLIBLE, SLANG_KIND_TASK,
    SLANG_KIND_COUNT
} slang_reflect_kind;

typedef struct slang_reflect_any {      /* a Slang `any`: 16 bytes */
    void *data;                         /* +0  payload of the box */
    const slang_reflect_type *type;     /* +8  dynamic type, NULL = nil */
} slang_reflect_any;

typedef struct slang_reflect_field {    /* std.reflect.FieldInfo, 72 bytes */
    slang_string *name;                 /* +0 */
    const slang_reflect_type *type;     /* +8 */
    int64_t offset;                     /* +16 byte offset in the struct (enum value for variants) */
    int64_t index;                      /* +24 */
    bool embedded;                      /* +32 */
    bool exported;                      /* +33 */
    slang_slice attrs;                  /* +40 []any, each holding a *A */
} slang_reflect_field;

typedef struct slang_reflect_method {   /* std.reflect.MethodInfo, 72 bytes */
    slang_string *name;                 /* +0 */
    const slang_reflect_type *type;     /* +8  func(*S, params) ret (no receiver if static) */
    bool is_static;                     /* +16 */
    void *invoke;                       /* +24 packed thunk: {any, error*} (any recv, []any args) */
    void *fnval;                        /* +32 closure-convention thunk fn(env, recv, args...) */
    slang_slice attrs;                  /* +40 */
} slang_reflect_method;

typedef struct slang_reflect_variant {  /* std.reflect.VariantInfo, 80 bytes */
    slang_string *name;                 /* +0 */
    int64_t index;                      /* +8 */
    slang_slice fields;                 /* +16 []*FieldInfo */
    slang_slice attrs;                  /* +48 */
} slang_reflect_variant;

struct slang_reflect_type {             /* std.reflect.TypeInfo, 232 bytes */
    slang_type_info rt;                 /* +0  name, size (16 + sizeof T), drop */
    int32_t kind;                       /* +24 slang_reflect_kind */
    slang_string *name;                 /* +32 short name: User, *User, []int */
    slang_string *full_name;            /* +40 app.models.User */
    int64_t size;                       /* +48 sizeof T */
    int64_t align;                      /* +56 */
    slang_slice fields;                 /* +64  []*FieldInfo (structs) */
    slang_slice methods;                /* +96  []*MethodInfo (full types only) */
    slang_slice variants;               /* +128 []*VariantInfo (enums) */
    const slang_reflect_type *elem;     /* +160 pointer/slice/array/weak/T!/Task element, map value */
    const slang_reflect_type *key;      /* +168 map key */
    int64_t len;                        /* +176 array length */
    slang_slice attrs;                  /* +184 []any */
    const slang_reflect_type *ptr_to;   /* +216 *T (full types only; TypeInfo.New) */
    const slang_reflect_type *canon;    /* +224 for a spelling alias (int64): the int table */
};

/* The table of a box (a heap object made by compiled code), or NULL. */
const slang_reflect_type *slang_reflect_type_of(const void *obj);
/* "Struct", "Int", ... (the std.reflect.Kind variant name). */
const char *slang_reflect_kind_name(int32_t kind);
/* Field / method by name ([borrowed] table entries), NULL if absent. */
const slang_reflect_field *slang_reflect_field_by_name(const slang_reflect_type *t, const char *name);
const slang_reflect_method *slang_reflect_method_by_name(const slang_reflect_type *t, const char *name);
/* The canonical table (follows `canon`); NULL for NULL. */
const slang_reflect_type *slang_reflect_canon(const slang_reflect_type *t);

/* std.encoding.json (std_json.c): tokens of a JSON text (see there) and
 * a quoted JSON string literal. */
void          slang_encoding_json_scan(const slang_string *text, slang_slice *out, slang_error **err);
slang_string *slang_encoding_json_quote(const slang_string *s);

/* std.diag / std.log runtime halves (diag.c, profile.c; ABI.md section 18). */
slang_object *slang_diag_rt_slots(void);                  /* map[string]any, +1 or NULL */
void          slang_diag_rt_set_slots(slang_object *m);
void          slang_diag_rt_lock(void);
void          slang_diag_rt_unlock(void);
int64_t       slang_diag_rt_live_objects(void);
int64_t       slang_diag_rt_allocations(void);
int64_t       slang_diag_rt_live_tasks(void);
int64_t       slang_diag_rt_queue_len(void);
int64_t       slang_diag_rt_workers(void);
int64_t       slang_diag_rt_mono_nanos(void);
int64_t       slang_diag_rt_heap_in_use(void);              /* object heap bytes (heap.c, ABI.md 3.2) */
int64_t       slang_diag_rt_heap_retained(void);
int64_t       slang_diag_rt_heap_released(void);
void          slang_runtime_free_os_memory(void);           /* std.runtime.FreeOSMemory */
/* std.testing.Panics: runs the closure on a thread in a panic capture
 * scope; 1 panicked, 0 returned, -1 unsupported (std_testing.c); the
 * message of the last caught panic of the calling thread (+1). */
int64_t       slang_testing_rt_capture(void *fn, void *env);
slang_string *slang_testing_rt_capture_message(void);
void          slang_diag_rt_start_cpu_profile(const slang_string *path, int64_t hz, slang_error **err);
int64_t       slang_diag_rt_stop_cpu_profile(slang_error **err);
slang_string *slang_diag_rt_heap_text(void);
void          slang_diag_rt_write_heap_profile(const slang_string *path, slang_error **err);
int64_t       slang_log_rt_open(const slang_string *path, slang_error **err);
void          slang_log_rt_write(int64_t fd, const slang_string *s, slang_error **err);
void          slang_log_rt_close(int64_t fd);
bool          slang_log_rt_isatty(int64_t fd);

/* ======================================================================
 * Debug agent (debug.c, std_unsafe.c, std_thread.c, std_plugin.c;
 * ABI.md section 22). The tables and frame records are described there;
 * compiled code passes them as opaque pointers.
 * ====================================================================== */

struct slang_dbg_frame;
struct slang_dbg_func;
/* Hooks emitted by `--debug-agent` builds, each behind `if (on)`, where
 * `on` is the hidden uint8_t `slang_dbg_on` loaded at function entry. */
void slang_dbg_enter(struct slang_dbg_frame *f, const struct slang_dbg_func *func, void *const *locals, int32_t entry_stmt);
void slang_dbg_stmt(struct slang_dbg_frame *f, int32_t stmt);
void slang_dbg_leave(struct slang_dbg_frame *f);
void slang_dbg_suspend(struct slang_dbg_frame *f);
void slang_dbg_resume(struct slang_dbg_frame *f);
/* C main: after slang_runtime_init (starts the agent when SLANG_DEBUG is
 * set) and before returning (reports the exit to the client). */
void slang_dbg_boot(const void *tables, void (*entry)(void));
void slang_dbg_exit(int32_t code);

/* std.debug internals (rt*). Closures are passed as their two words. */
int64_t       slang_debug_rt_tables(void);
void          slang_debug_rt_set_hook(void *fn, void *env);
void          slang_debug_rt_set_exit_hook(void *fn, void *env);
void          slang_debug_rt_set_bp(int64_t stmt, bool on);
void          slang_debug_rt_on_error(bool on);
void          slang_debug_rt_stop_next(void);
void          slang_debug_rt_freeze(bool on);
bool          slang_debug_rt_resume(int64_t task, int64_t mode, bool single);
void          slang_debug_rt_release_all(void);
void          slang_debug_rt_tasks(slang_slice *out);
int64_t       slang_debug_rt_task_state(int64_t task);
int64_t       slang_debug_rt_task_frame(int64_t task);
int64_t       slang_debug_rt_task_func(int64_t task);
int64_t       slang_debug_rt_task_reason(int64_t task);
void          slang_debug_rt_lock(int64_t n);
void          slang_debug_rt_unlock(int64_t n);
void          slang_debug_rt_wait_ready(void);
void          slang_debug_rt_signal_ready(void);
void          slang_debug_rt_ignore_hooks(void);
slang_string *slang_debug_rt_panic_text(void);
int64_t       slang_debug_rt_cur_error(void);
slang_string *slang_debug_rt_error_text(int64_t addr);
slang_string *slang_debug_rt_type_name(int64_t addr);
int64_t       slang_debug_rt_await_int(slang_task *t);
bool          slang_debug_rt_task_done(int64_t addr);
int64_t       slang_debug_rt_task_slot(int64_t addr);
slang_string *slang_debug_rt_sym_name(int64_t addr);
SLANG_NORETURN void slang_debug_rt_exit(int64_t code);
int64_t       slang_debug_rt_now_ms(void);
slang_string *slang_debug_rt_string(int64_t addr);
int64_t       slang_debug_rt_map_len(int64_t m);
void          slang_debug_rt_map_entries(int64_t m, int64_t start, int64_t n, slang_slice *out);
slang_string *slang_debug_rt_error_type(int64_t addr);
int64_t       slang_debug_rt_listen(const slang_string *host, int64_t port, slang_error **err);
int64_t       slang_debug_rt_sock_port(int64_t fd);
int64_t       slang_debug_rt_accept(int64_t fd);
void          slang_debug_rt_recv(int64_t fd, int64_t max, slang_slice *out);
bool          slang_debug_rt_send(int64_t fd, const slang_string *s);
void          slang_debug_rt_close(int64_t fd);
void          slang_debug_rt_eprint(const slang_string *s);

/* std.unsafe (capability `unsafe`). */
int64_t       slang_unsafe_read_int(int64_t addr, int64_t size);
int64_t       slang_unsafe_read_uint(int64_t addr, int64_t size);
double        slang_unsafe_read_float(int64_t addr, int64_t size);
void          slang_unsafe_write_int(int64_t addr, int64_t size, int64_t v);
void          slang_unsafe_read_bytes(int64_t addr, int64_t n, slang_slice *out);
void          slang_unsafe_write_bytes(int64_t addr, const slang_slice *b);
slang_string *slang_unsafe_c_string(int64_t addr);
int64_t       slang_unsafe_addr_of(void *data, void *type);
int64_t       slang_unsafe_type_of(void *data, void *type);
int64_t       slang_unsafe_call_func(int64_t addr, const slang_slice *args);

/* std.thread. */
int64_t       slang_thread_spawn(void *fn, void *env);
void          slang_thread_join(int64_t h);
void          slang_thread_sleep(int64_t ms);

/* std.plugin (capability `plugin`). */
int64_t       slang_plugin_rt_open(const slang_string *path, slang_error **err);
int64_t       slang_plugin_rt_sym(int64_t h, const slang_string *name, slang_error **err);
void          slang_plugin_rt_close(int64_t h);

#ifdef __cplusplus
}
#endif

#endif /* SLANG_RT_H */
