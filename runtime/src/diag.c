/* diag.c - runtime helpers of the Slang-source std.diag and std.log
 * (compiler/src/sema/stdlib_src/{diag,log}.sl): a process-wide slot map
 * for their global state (the default logger, the metrics registry, the
 * tracer), a spin lock guarding it, runtime metrics, and plain file
 * descriptors for the log file sink. ABI.md section 18. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ---- process-wide slots ----------------------------------------------- */

static slang_object *g_slots; /* map[string]any, owned; NULL until first set */
static int g_slots_lock;

slang_object *slang_diag_rt_slots(void) {
    slang_object *m = __atomic_load_n(&g_slots, __ATOMIC_ACQUIRE);
    slang_retain(m);
    return m;
}

void slang_diag_rt_set_slots(slang_object *m) {
    slang_retain(m);
    slang_object *old = __atomic_exchange_n(&g_slots, m, __ATOMIC_ACQ_REL);
    slang_release(old);
}

void slang_diag_rt_lock(void) {
    while (__atomic_exchange_n(&g_slots_lock, 1, __ATOMIC_ACQUIRE)) slang__plat_yield();
}

void slang_diag_rt_unlock(void) { __atomic_store_n(&g_slots_lock, 0, __ATOMIC_RELEASE); }

void slang__diag_shutdown(void) {
    slang_object *old = __atomic_exchange_n(&g_slots, (slang_object *)NULL, __ATOMIC_ACQ_REL);
    slang_release(old);
}

/* ---- runtime metrics -------------------------------------------------- */

int64_t slang_diag_rt_live_objects(void) { return slang_live_objects(); }
int64_t slang_diag_rt_allocations(void) { return slang__total_objects(); }
int64_t slang_diag_rt_live_tasks(void) { return slang__live_tasks(); }
int64_t slang_diag_rt_queue_len(void) { return slang__sched_queue_len(); }
int64_t slang_diag_rt_workers(void) { return slang_sched_workers(); }
int64_t slang_diag_rt_mono_nanos(void) { return slang__plat_mono_ns(); }

/* ---- log file sink ---------------------------------------------------- */

static void file_err(slang_error **err, const char *op, const slang_string *path, int e) {
    if (!err) return;
    char buf[600];
    snprintf(buf, sizeof buf, "%s %.400s: %s", op, slang_string_cstr(path), strerror(e));
    *err = slang_error_new_cstr(buf);
}

int64_t slang_log_rt_open(const slang_string *path, slang_error **err) {
    int64_t fd = slang__plat_open(slang_string_cstr(path), SLANG_PLAT_O_WRONLY | SLANG_PLAT_O_CREATE | SLANG_PLAT_O_APPEND, 0644);
    if (fd < 0) {
        file_err(err, "open", path, (int)-fd);
        return -1;
    }
    if (err) *err = NULL;
    return fd;
}

void slang_log_rt_write(int64_t fd, const slang_string *s, slang_error **err) {
    const char *p = slang_string_cstr(s);
    int64_t n = slang_string_len(s);
    int e = slang__plat_write_all(fd, p, n);
    if (e) {
        if (err) {
            char buf[128];
            snprintf(buf, sizeof buf, "write log file: %s", strerror(e));
            *err = slang_error_new_cstr(buf);
        }
        return;
    }
    if (err) *err = NULL;
}

void slang_log_rt_close(int64_t fd) {
    if (fd >= 0) slang__plat_close(fd);
}

bool slang_log_rt_isatty(int64_t fd) { return slang__plat_isatty(fd); }
