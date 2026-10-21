/* Internal helpers shared between runtime source files. Not part of the ABI. */
#ifndef SLANG_RT_INTERNAL_H
#define SLANG_RT_INTERNAL_H

#include "slang_rt.h"

#define SLANG_HIDDEN __attribute__((visibility("hidden")))

SLANG_HIDDEN SLANG_NORETURN void slang__panicf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Allocate a string object with room for len bytes (+NUL); data is zeroed. */
SLANG_HIDDEN slang_string *slang__string_alloc(int64_t len);

/* Format a double the way Slang prints it (shortest round-trip). buf >= 32. */
SLANG_HIDDEN int slang__format_float(char *buf, size_t cap, double v);

/* Encode a rune as UTF-8 into buf (>= 4 bytes); returns number of bytes. */
SLANG_HIDDEN int slang__utf8_encode(char *buf, int32_t r);

/* Scheduler bootstrap (task.c), used by main.c. slang__process_init does
 * the process-wide setup only; the workers start with the first task. */
SLANG_HIDDEN void slang__process_init(void);
SLANG_HIDDEN void slang__sched_start(void);
SLANG_HIDDEN void slang__sched_stop(void);
/* Stops the I/O poller thread (task_io.c); pending waits are dropped. */
SLANG_HIDDEN void slang__io_stop(void);
/* Shutdown hooks, set by the subsystems when they start (so a program that
 * never does I/O does not link their stop code): slang__io_stop (task_io.c)
 * and slang__net_stop (std_net.c). Defined in task.c. */
SLANG_HIDDEN extern void (*slang__io_stop_fn)(void);
SLANG_HIDDEN extern void (*slang__net_stop_fn)(void);
/* Debug agent hooks (debug.c), set by slang_dbg_boot. */
SLANG_HIDDEN extern void (*slang__dbg_panic_fn)(const char *msg, int64_t len);
SLANG_HIDDEN extern void (*slang__dbg_error_fn)(const slang_error *err);
/* Runs fn(w) when the current task is cancelled (task.c, ABI.md 10.11). */
SLANG_HIDDEN void slang__cancel_hook(void (*fn)(slang_task *), slang_task *w);
/* The same with a word of data passed to fn (task_io.c: the fd of the wait). */
SLANG_HIDDEN void slang__cancel_hook_data(void (*fn)(slang_task *, uintptr_t), slang_task *w, uintptr_t data);
/* Stops the resolver pool (std_net.c); called by slang__io_stop. Queued
 * lookups are dropped without completing, like pending I/O waits. */
SLANG_HIDDEN void slang__net_stop(void);
/* task_io.c introspection (tests, std.diag): the shard owning fd (-1 if
 * none) and the number of poller shards running (0 before the first wait). */
SLANG_HIDDEN int64_t slang__io_shard_of(int64_t fd);
SLANG_HIDDEN int64_t slang__io_shards(void);

/* Arguments registry (main.c). */
SLANG_HIDDEN void slang__io_flush_all(void);

/* Object lifecycle helpers (mem.c, weak.c). */
SLANG_HIDDEN void slang__object_immortalize(slang_object *o);
/* Usable bytes of an object's allocation (header included), and resizing of
 * a uniquely referenced object (it may move; new bytes are not zeroed). */
SLANG_HIDDEN int64_t slang__object_capacity(slang_object *o);
SLANG_HIDDEN slang_object *slang__object_resize(slang_object *o, int64_t size);
/* Like slang_object_new, but the payload (after the header) is NOT zeroed. */
SLANG_HIDDEN slang_object *slang__object_new_uninit(const slang_type_info *type, int64_t size);
/* Allocate a string object of len bytes; only len and the NUL are set. */
SLANG_HIDDEN slang_string *slang__string_alloc_uninit(int64_t len);
/* The byte written at data[len] of a new owned string: '\0', or a non-NUL
 * byte in the SLANG_STRESS_NONUL test build, which makes every string look
 * like a view to find code that wrongly relies on the terminator. */
#ifdef SLANG_STRESS_NONUL
#define SLANG__STR_END '~'
#else
#define SLANG__STR_END '\0'
#endif
/* Views (string.c, ABI.md 4.1). */
typedef struct slang__sview { slang_string s; slang_object *owner; } slang__sview;
typedef struct slang__vblock {
    slang_object hdr;
    slang_object *owner;       /* bytes owner (+1), NULL for static bytes */
    int64_t n, cap;            /* views made / room */
    slang_string v[];
} slang__vblock;
/* s[lo:hi] (bounds already checked): a standalone view, or a copy when the
 * piece is short or s cannot be viewed. */
SLANG_HIDDEN slang_string *slang__string_sub(const slang_string *s, int64_t lo, int64_t hi);
/* A block for up to cap views of parent's bytes (+1 to the caller), or
 * NULL when parent cannot be viewed; then _add copies. _add returns a +1
 * string of p[0..n) (NULL for n == 0; p must lie in parent's bytes); _done
 * drops the builder's reference. A block must not be shared before _done. */
SLANG_HIDDEN slang__vblock *slang__vblock_new(const slang_string *parent, int64_t cap);
SLANG_HIDDEN slang_string *slang__vblock_add(slang__vblock *b, const char *p, int64_t n);
SLANG_HIDDEN void slang__vblock_done(slang__vblock *b);
/* First occurrence of needle[0..m) in hay[0..n), or NULL (m > 0). */
SLANG_HIDDEN const char *slang__memmem(const char *hay, int64_t n, const char *needle, int64_t m);
SLANG_HIDDEN void slang__weak_clear(slang_object *o);
SLANG_HIDDEN void slang__leak_report(void);
/* Leak check mode 2: calls cb on every live object (mem.c), and names the
 * pointer fields that close reference cycles among them (leak.c). */
SLANG_HIDDEN void slang__live_each(void (*cb)(slang_object *obj, void *arg), void *arg);
SLANG_HIDDEN void slang__leak_cycles(void);
/* std.diag (mem.c): objects allocated so far, and a walk over the live
 * objects (type name, allocation size) while leak tracking with type names
 * is on (`--leak-check` mode 2; false otherwise). `cb` must not allocate
 * Slang objects. */
SLANG_HIDDEN int64_t slang__total_objects(void);
SLANG_HIDDEN bool slang__heap_each(void (*cb)(const char *type, int64_t bytes, void *arg), void *arg);
/* std.diag (task.c): live task objects and jobs waiting in the run queue. */
SLANG_HIDDEN int64_t slang__live_tasks(void);
SLANG_HIDDEN int64_t slang__sched_queue_len(void);
/* std.diag (diag.c): releases the process-wide slots before the leak report. */
SLANG_HIDDEN void slang__diag_shutdown(void);

/* Debug agent (debug.c, ABI.md 22): the hook flag, the panic and error
 * hooks, and task.c's view of the per-task debug records. */
SLANG_HIDDEN extern uint8_t slang_dbg_on;
SLANG_HIDDEN void slang__dbg_panic(const char *msg, int64_t len);
SLANG_HIDDEN void slang__dbg_error(const slang_error *err);
SLANG_HIDDEN int slang__dbg_error_wanted(void);
SLANG_HIDDEN void slang__dbg_task_drop(int32_t id);
SLANG_HIDDEN slang_task *slang__task_cur(void);
SLANG_HIDDEN int32_t slang__task_dbg_id(const slang_task *t);
SLANG_HIDDEN void slang__task_set_dbg_id(slang_task *t, int32_t id);

#define SLANG_ROUND8(n) (((n) + 7) & ~(int64_t)7)

#endif
