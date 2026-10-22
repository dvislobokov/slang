/* std_testing.c - testing.Panics without a child process (ABI.md 8 and
 * 14.4, docs/TOOLING.md "Тесты"): the function runs on a dedicated OS
 * thread inside a *panic capture scope*. A panic on that thread while the
 * scope is active does not end the process: die() (panic.c) saves the
 * message and jumps back to the scope's start, the thread ends and the
 * caller learns that the function panicked (setjmp/longjmp: no unwinding,
 * so nothing runs in between). No defer and no destructor of
 * the abandoned frames runs, and what they owned is leaked (the test
 * process is short-lived); locks the function held stay held. Panics on
 * other threads (tasks the function started) still end the process.
 * Platforms without the capture (ZELL_PLAT_HAS_PANIC_CAPTURE 0) return
 * -1, and testing falls back to re-running the test in a child. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <string.h>

_Thread_local zell__panic_capture *zell__panic_capture_tls;

typedef void (*closure0)(void *env);

typedef struct {
    closure0 fn;
    void *env;
    int panicked;
    zell__panic_capture cap;
} capture_run;

#if ZELL_PLAT_HAS_PANIC_CAPTURE
/* Separate from the setjmp frame so the jump target stays valid. */
static __attribute__((noinline)) void call_fn(capture_run *r) { r->fn(r->env); }

static void *capture_main(void *arg) {
    capture_run *r = arg;
    zell__plat_thread_stack_init();
    if (setjmp(r->cap.jb) == 0) {
        zell__panic_capture_tls = &r->cap;
        call_fn(r);
        zell__panic_capture_tls = NULL;
        r->panicked = 0;
    } else {
        zell__panic_capture_tls = NULL;
        r->panicked = 1;
    }
    return NULL;
}
#endif

static _Thread_local zell_string *t_last;   /* message of the last captured panic */

/* Runs the closure (fn, env) on a new thread in a capture scope: 1 if it
 * panicked (its message: zell_testing_rt_capture_message), 0 if it
 * returned, -1 if this platform cannot capture panics. */
int64_t zell_testing_rt_capture(void *fn, void *env) {
    zell_release(t_last);
    t_last = NULL;
#if ZELL_PLAT_HAS_PANIC_CAPTURE
    if (!fn) zell_panic_nil();
    capture_run r;
    memset(&r, 0, sizeof r);
    r.fn = (closure0)fn;
    r.env = env;   /* borrowed: the caller keeps the closure alive */
    zell__plat_thread th;
    if (!zell__plat_thread_start(&th, capture_main, &r)) return -1;
    zell__plat_thread_join(th);
    if (r.panicked) t_last = zell_string_new(r.cap.msg ? r.cap.msg : "", r.cap.len);
    free(r.cap.msg);
    return r.panicked;
#else
    (void)fn;
    (void)env;
    return -1;
#endif
}

/* The message of the last panic zell_testing_rt_capture caught on this
 * thread ("" if none), +1. */
zell_string *zell_testing_rt_capture_message(void) {
    if (!t_last) return zell_string_new("", 0);
    zell_retain(t_last);
    return t_last;
}
