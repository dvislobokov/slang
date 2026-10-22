/* std_thread.c - thread: OS threads outside the task scheduler
 * (runtime/ABI.md section 22.6). The debug agent runs on one, so it keeps
 * answering while every task is paused. A thread runs a Zell closure
 * `func()`; it is no scheduler worker, so `await` inside it blocks. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdlib.h>

typedef void (*closure0)(void *env);

typedef struct thread_rec {
    zell__plat_thread th;
    closure0 fn;
    void *env;      /* owned */
    int joined;
} thread_rec;

static void *thread_main(void *arg) {
    thread_rec *r = arg;
    zell__plat_thread_stack_init();
    r->fn(r->env);
    return NULL;
}

/* thread.Spawn(f func()) int: starts f on a new thread; the handle is
 * for Join. The closure is passed as its two words (fn, env). */
int64_t zell_thread_spawn(void *fn, void *env) {
    if (!fn) zell_panic_nil();
    thread_rec *r = calloc(1, sizeof *r);
    if (!r) zell_panic("out of memory (thread)");
    r->fn = (closure0)fn;
    zell_retain(env);
    r->env = env;
    if (!zell__plat_thread_start(&r->th, thread_main, r)) zell_panic("cannot create thread");
    return (int64_t)(intptr_t)r;
}

/* Waits for the thread to finish (once). */
void zell_thread_join(int64_t h) {
    thread_rec *r = (thread_rec *)(intptr_t)h;
    if (!r || r->joined) return;
    r->joined = 1;
    zell__plat_thread_join(r->th);
    zell_release(r->env);
    free(r);
}

/* Blocks the calling OS thread for ms milliseconds. */
void zell_thread_sleep(int64_t ms) {
    if (ms <= 0) return;
    zell__plat_sleep_ns(ms * 1000000);
}
