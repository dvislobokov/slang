/* std_thread.c - std.thread: OS threads outside the task scheduler
 * (runtime/ABI.md section 22.6). The debug agent runs on one, so it keeps
 * answering while every task is paused. A thread runs a Slang closure
 * `func()`; it is no scheduler worker, so `await` inside it blocks. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <time.h>

typedef void (*closure0)(void *env);

typedef struct thread_rec {
    slang__plat_thread th;
    closure0 fn;
    void *env;      /* owned */
    int joined;
} thread_rec;

static void *thread_main(void *arg) {
    thread_rec *r = arg;
    slang__plat_thread_stack_init();
    r->fn(r->env);
    return NULL;
}

/* std.thread.Spawn(f func()) int: starts f on a new thread; the handle is
 * for Join. The closure is passed as its two words (fn, env). */
int64_t slang_thread_spawn(void *fn, void *env) {
    if (!fn) slang_panic_nil();
    thread_rec *r = calloc(1, sizeof *r);
    if (!r) slang_panic("out of memory (thread)");
    r->fn = (closure0)fn;
    slang_retain(env);
    r->env = env;
    if (!slang__plat_thread_start(&r->th, thread_main, r)) slang_panic("cannot create thread");
    return (int64_t)(intptr_t)r;
}

/* Waits for the thread to finish (once). */
void slang_thread_join(int64_t h) {
    thread_rec *r = (thread_rec *)(intptr_t)h;
    if (!r || r->joined) return;
    r->joined = 1;
    slang__plat_thread_join(r->th);
    slang_release(r->env);
    free(r);
}

/* Blocks the calling OS thread for ms milliseconds. */
void slang_thread_sleep(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) {}
}
