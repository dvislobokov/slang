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

/* Scheduler bootstrap (task.c), used by main.c. */
SLANG_HIDDEN void slang__sched_start(void);
SLANG_HIDDEN void slang__sched_stop(void);
/* Stops the I/O poller thread (task_io.c); pending waits are dropped. */
SLANG_HIDDEN void slang__io_stop(void);
/* Runs fn(w) when the current task is cancelled (task.c, ABI.md 10.11). */
SLANG_HIDDEN void slang__cancel_hook(void (*fn)(slang_task *), slang_task *w);

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
/* First occurrence of needle[0..m) in hay[0..n), or NULL (m > 0). */
SLANG_HIDDEN const char *slang__memmem(const char *hay, int64_t n, const char *needle, int64_t m);
SLANG_HIDDEN void slang__weak_clear(slang_object *o);
SLANG_HIDDEN void slang__leak_report(void);

#define SLANG_ROUND8(n) (((n) + 7) & ~(int64_t)7)

#endif
