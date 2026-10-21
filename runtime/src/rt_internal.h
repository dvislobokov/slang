/* Internal helpers shared between runtime source files. Not part of the ABI. */
#ifndef SLANG_RT_INTERNAL_H
#define SLANG_RT_INTERNAL_H

#include "slang_rt.h"

#include <setjmp.h>

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
/* The object heap (heap.c; the allocation entry points are in heap.h).
 * collect: take back this thread's remotely freed pages; with `force`,
 * also retire its empty pages and give every free page and cached mapping
 * back to the OS (runtime.FreeOSMemory). stats: bytes in use (owned pages,
 * medium and large objects), retained (free but still backed by memory),
 * released (free pages returned to the OS whose address range is kept),
 * returned (cumulative bytes given back to the OS). */
#define SLANG__HP_CLASSES 36
#define SLANG__HP_SMALL_MAX 16384
SLANG_HIDDEN void slang__heap_collect(int force);
/* Biased reference counting (rc.c, ABI.md 3.1). thread_enable gives the
 * calling thread an owner slot (workers, the main thread); retain/release
 * of biased and of unowned words while threaded (mem.c dispatches); count:
 * the count a word stands for (negative words unchanged); unique: the
 * caller holds the only reference (in-place updates); try_retain_biased:
 * retain unless the count is zero (weak.c); poll merges the calling
 * thread's queue (pending: it is not empty); park/unpark bracket a
 * blocking wait of a slot thread, during which others may merge its queue
 * (park(1) merges first, so it must not be called with runtime locks held
 * that a destructor could need; park(0) only gives the slot lock away);
 * drain_all merges every queue
 * (shutdown, before the leak report). */
SLANG_HIDDEN void slang__rc_thread_enable(void);
/* Marks the calling thread as one that may own objects (it gets a slot at
 * its first biasing): the main thread (slang_runtime_init) and workers. */
SLANG_HIDDEN void slang__rc_thread_eligible(void);
SLANG_HIDDEN void slang__rc_retain_biased(slang_object *o, int64_t w);
SLANG_HIDDEN void slang__rc_release_biased(slang_object *o, int64_t w);
SLANG_HIDDEN void slang__rc_retain_unowned(slang_object *o, int64_t w);
SLANG_HIDDEN void slang__rc_release_unowned(slang_object *o, int64_t w);
SLANG_HIDDEN int64_t slang__rc_count(int64_t w);
SLANG_HIDDEN int slang__rc_unique(const slang_object *o);
SLANG_HIDDEN int slang__rc_try_retain_biased(slang_object *o);
SLANG_HIDDEN void slang__rc_poll(void);
SLANG_HIDDEN int slang__rc_pending(void);
SLANG_HIDDEN void slang__rc_park(int merge);
SLANG_HIDDEN void slang__rc_unpark(void);
SLANG_HIDDEN void slang__rc_drain_all(void);
/* Whether slang__heap_collect(0) has work on this thread (idle workers). */
SLANG_HIDDEN int slang__heap_needs_collect(void);
SLANG_HIDDEN void slang__heap_stats(int64_t *in_use, int64_t *retained, int64_t *released, int64_t *returned);
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
/* The weak table lock, and clearing an object's box with it held (cycles.c). */
SLANG_HIDDEN void slang__weak_lock(void);
SLANG_HIDDEN void slang__weak_unlock(void);
SLANG_HIDDEN void slang__weak_clear_locked(slang_object *o);
/* Cycle collector (cycles.c, ABI.md 3.8). retain/release of an object with
 * SLANG_RC_CYCLIC go here (mem.c); _dirty notes a count change of a MARKED
 * object made by an atomic increment elsewhere (mem.c, weak.c); _flag sets
 * SLANG_RC_CYCLIC on a fresh object unless the collector is off (slice.c
 * backings, map.c maps); _at_exit runs before the leak report: a final
 * collection in auto mode (returns 0), else a dry run counting the leaked
 * objects the collector could free (-1 when it is off). */
/* These are weak references outside cycles.c: a program that never
 * allocates an object of a cyclic type does not link the collector (only
 * flagged objects reach _retain/_release/_dirty, and a flagged object
 * exists only once cycles.c is linked; the others are tested for NULL). */
#define SLANG_CYC_WEAK __attribute__((weak))
SLANG_HIDDEN SLANG_CYC_WEAK void slang__cycles_retain(slang_object *o);
SLANG_HIDDEN SLANG_CYC_WEAK void slang__cycles_release(slang_object *o);
/* A release that never records a possible root (slang_ptr_release_noroot). */
SLANG_HIDDEN SLANG_CYC_WEAK void slang__cycles_release_noroot(slang_object *o);
SLANG_HIDDEN SLANG_CYC_WEAK void slang__cycles_dirty(slang_object *o);
SLANG_HIDDEN SLANG_CYC_WEAK void slang__cycles_flag(slang_object *o);
/* Bytes to allocate for an object of `size` that slang__cycles_flag will
 * flag: room for the collector's hidden word after it (ABI.md 3.8). */
SLANG_HIDDEN SLANG_CYC_WEAK int64_t slang__cycles_extra(int64_t size);
SLANG_HIDDEN SLANG_CYC_WEAK int64_t slang__cycles_at_exit(void);
/* Runtime containers (slice.c, map.c): their type infos and visitors. */
SLANG_HIDDEN extern const slang_type_info slang__backing_type;
SLANG_HIDDEN void slang__backing_visit(void *obj, slang_cycle_cb cb, void *ctx);
SLANG_HIDDEN extern const slang_type_info slang__map_type;
SLANG_HIDDEN void slang__map_visit(void *obj, slang_cycle_cb cb, void *ctx);
/* mem.c, for cycles.c: free an object's memory (stats: one live object
 * less), and move one object out of / back into the live count (an object
 * whose Drop ran but whose memory is kept for a while). */
SLANG_HIDDEN void slang__object_free(slang_object *o);
/* Per-type live counters for std.diag (diag_heap.c): on with
 * SLANG_DIAG_HEAP=1; mem.c counts every allocation (+1) and free (-1). */
/* Weak: diag_heap.c is linked only by programs that use std.diag's heap
 * report, so mem.c tests the flag's address first (SLANG_DIAG_HEAP_ON). */
SLANG_HIDDEN SLANG_CYC_WEAK extern uint8_t slang__diag_heap_on;
SLANG_HIDDEN SLANG_CYC_WEAK void slang__diag_heap_count(const slang_type_info *type, int64_t d);
#define SLANG_DIAG_HEAP_ON() __builtin_expect(&slang__diag_heap_on != NULL && slang__diag_heap_on, 0)
SLANG_HIDDEN void slang__object_uncount(void);
SLANG_HIDDEN void slang__object_recount(void);
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

/* Panic capture scopes (std_testing.c): while a thread's scope is set, a
 * panic on that thread saves its message (malloc'd) and longjmps to `jb`
 * instead of ending the process (panic.c). */
typedef struct slang__panic_capture {
    jmp_buf jb;
    char *msg;
    int64_t len;
} slang__panic_capture;
SLANG_HIDDEN extern _Thread_local slang__panic_capture *slang__panic_capture_tls;

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
