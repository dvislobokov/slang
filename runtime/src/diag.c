/* diag.c - runtime helpers of the Zell-source std.diag and std.log
 * (compiler/src/sema/stdlib_src/{diag,log}.zl): a process-wide slot map
 * for their global state (the default logger, the metrics registry, the
 * tracer), a spin lock guarding it, runtime metrics, and plain file
 * descriptors for the log file sink. ABI.md section 18. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ---- process-wide slots ----------------------------------------------- */

static zell_object *g_slots; /* map[string]any, owned; NULL until first set */
static int g_slots_lock;      /* rtLock / rtUnlock (Zell-side critical sections) */
static int g_ptr_lock;        /* g_slots itself: read+retain vs. exchange */

static void ptr_lock(void) {
    while (__atomic_exchange_n(&g_ptr_lock, 1, __ATOMIC_ACQUIRE)) zell__plat_yield();
}
static void ptr_unlock(void) { __atomic_store_n(&g_ptr_lock, 0, __ATOMIC_RELEASE); }

zell_object *zell_diag_rt_slots(void) {
    ptr_lock();
    zell_object *m = g_slots;
    zell_retain(m);
    ptr_unlock();
    return m;
}

void zell_diag_rt_set_slots(zell_object *m) {
    zell_retain(m);
    ptr_lock();
    zell_object *old = g_slots;
    g_slots = m;
    ptr_unlock();
    zell_release(old);
}

void zell_diag_rt_lock(void) {
    while (__atomic_exchange_n(&g_slots_lock, 1, __ATOMIC_ACQUIRE)) zell__plat_yield();
}

void zell_diag_rt_unlock(void) { __atomic_store_n(&g_slots_lock, 0, __ATOMIC_RELEASE); }

/* The slot map is the registry of process-wide singletons: the default
 * http.Transport, the default logger, the metrics registry, the tracer.
 * zell_runtime_shutdown releases it before it stops the scheduler, so
 * their destructors (closing pooled connections, flushing sinks) and the
 * tasks those wake (HTTP/2 readers) finish before the leak report, and
 * once more after (whatever was registered again meanwhile). */
void zell__diag_shutdown(void) {
    ptr_lock();
    zell_object *old = g_slots;
    g_slots = NULL;
    ptr_unlock();
    zell_release(old);
}

/* ---- runtime metrics -------------------------------------------------- */

int64_t zell_diag_rt_live_objects(void) { return zell_live_objects(); }
int64_t zell_diag_rt_allocations(void) { return zell__total_objects(); }
int64_t zell_diag_rt_live_tasks(void) { return zell__live_tasks(); }
int64_t zell_diag_rt_queue_len(void) { return zell__sched_queue_len(); }
int64_t zell_diag_rt_workers(void) { return zell_sched_workers(); }
int64_t zell_diag_rt_mono_nanos(void) { return zell__plat_mono_ns(); }

int64_t zell_diag_rt_heap_in_use(void) {
    int64_t v;
    zell__heap_stats(&v, NULL, NULL, NULL);
    return v;
}

int64_t zell_diag_rt_heap_retained(void) {
    int64_t v;
    zell__heap_stats(NULL, &v, NULL, NULL);
    return v;
}

int64_t zell_diag_rt_heap_released(void) {
    int64_t v;
    zell__heap_stats(NULL, NULL, &v, NULL);
    return v;
}

/* std.runtime.FreeOSMemory */
void zell_runtime_free_os_memory(void) { zell__heap_collect(1); }

/* ---- log file sink ---------------------------------------------------- */

static void file_err(zell_error **err, const char *op, const zell_string *path, int e) {
    if (!err) return;
    char buf[600];
    snprintf(buf, sizeof buf, "%s %.400s: %s", op, zell_string_cstr(path), strerror(e));
    *err = zell_error_new_cstr(buf);
}

int64_t zell_log_rt_open(const zell_string *path, zell_error **err) {
    int64_t fd = zell__plat_open(zell_string_cstr(path), ZELL_PLAT_O_WRONLY | ZELL_PLAT_O_CREATE | ZELL_PLAT_O_APPEND, 0644);
    if (fd < 0) {
        file_err(err, "open", path, (int)-fd);
        return -1;
    }
    if (err) *err = NULL;
    return fd;
}

void zell_log_rt_write(int64_t fd, const zell_string *s, zell_error **err) {
    const char *p = zell_string_cstr(s);
    int64_t n = zell_string_len(s);
    int e = zell__plat_write_all(fd, p, n);
    if (e) {
        if (err) {
            char buf[128];
            snprintf(buf, sizeof buf, "write log file: %s", strerror(e));
            *err = zell_error_new_cstr(buf);
        }
        return;
    }
    if (err) *err = NULL;
}

void zell_log_rt_close(int64_t fd) {
    if (fd >= 0) zell__plat_close(fd);
}

bool zell_log_rt_isatty(int64_t fd) { return zell__plat_isatty(fd); }
