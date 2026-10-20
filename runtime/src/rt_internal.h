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

/* Arguments registry (main.c). */
SLANG_HIDDEN void slang__io_flush_all(void);

/* Object lifecycle helpers (mem.c, weak.c). */
SLANG_HIDDEN void slang__object_immortalize(slang_object *o);
/* Usable bytes of an object's allocation (header included), and resizing of
 * a uniquely referenced object (it may move; new bytes are not zeroed). */
SLANG_HIDDEN int64_t slang__object_capacity(slang_object *o);
SLANG_HIDDEN slang_object *slang__object_resize(slang_object *o, int64_t size);
SLANG_HIDDEN void slang__weak_clear(slang_object *o);
SLANG_HIDDEN void slang__leak_report(void);

#define SLANG_ROUND8(n) (((n) + 7) & ~(int64_t)7)

#endif
