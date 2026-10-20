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
/* Bit 62 of a (non-immortal) refcount word: the object has a weak box.
 * The count itself is refcount & SLANG_RC_COUNT_MASK. */
#define SLANG_RC_WEAK_FLAG ((int64_t)1 << 62)
#define SLANG_RC_COUNT_MASK (SLANG_RC_WEAK_FLAG - 1)
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
typedef struct slang_value_ops {
    void (*retain)(void *slot);    /* +0 add the references held by *slot */
    void (*release)(void *slot);   /* +8 drop the references held by *slot */
} slang_value_ops;                 /* 16 bytes */

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
int64_t slang_refcount(const void *obj);      /* debugging / tests; 0 for NULL */
int64_t slang_live_objects(void);             /* number of live slang_object_new objects */
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
    int64_t len;        /* +16 length in bytes, excluding the trailing NUL */
    char data[];        /* +24 len bytes of UTF-8 followed by a NUL byte */
} slang_string;

extern const slang_type_info slang_type_string;

/* Static literal for C code. The compiler emits the equivalent LLVM global. */
#define SLANG_STATIC_STRING(name, lit)                                         \
    static struct { slang_object hdr; int64_t len; char data[sizeof(lit)]; }   \
        name##_storage = { { SLANG_RC_IMMORTAL, &slang_type_string },          \
                           (int64_t)(sizeof(lit) - 1), lit };                  \
    static slang_string *const name = (slang_string *)&name##_storage

slang_string *slang_string_new(const char *bytes, int64_t len);   /* owned copy */
slang_string *slang_string_from_cstr(const char *cstr);           /* owned copy */
slang_string *slang_string_literal(const char *bytes, int64_t len); /* immortal copy, never freed */
const char   *slang_string_data(const slang_string *s);  /* [borrowed] NUL-terminated, "" for NULL */
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
void   *slang_map_get_int(const slang_map *m, int64_t key);
void    slang_map_set_int(slang_map *m, int64_t key, const void *val);
bool    slang_map_delete_int(slang_map *m, int64_t key);
void    slang_map_iter_init(slang_map_iter *it, slang_map *m);
bool    slang_map_iter_next(slang_map_iter *it, void **key, void **val);

/* ======================================================================
 * Errors
 * ====================================================================== */

typedef slang_string *(*slang_message_fn)(void *payload); /* returns OWNED string */

typedef struct slang_error {
    slang_object hdr;                 /* +0  */
    slang_string *message;            /* +16 own message / wrap context (owned ref) */
    struct slang_error *inner;        /* +24 wrapped error (owned ref) or NULL */
    const slang_type_info *user_type; /* +32 type of boxed user error or NULL */
    void *payload;                    /* +40 boxed user value: slang object (owned ref) or NULL */
    slang_message_fn message_fn;      /* +48 user Message() thunk or NULL */
} slang_error;                        /* 56 bytes */

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

#ifdef __cplusplus
}
#endif

#endif /* SLANG_RT_H */
