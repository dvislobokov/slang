/* Tests for the platform layer (src/platform/), the I/O readiness waits
 * (task_io.c), the std.net wrappers (std_net.c) and the task primitives
 * added for std.task (context slot, signals, spin locks, yield). */
#include "test.h"
#include "../src/platform/platform.h"
#include <sys/socket.h>
#include <time.h>

static int64_t now_ms(void) { return slang__plat_mono_ns() / 1000000; }

static int64_t wait_code(slang_task *t) {
    int64_t r = *(int64_t *)slang_task_await(t);
    slang_release(t);
    return r;
}

/* ---- platform basics ---- */

static void *thread_fn(void *arg) {
    *(int *)arg = 42;
    return NULL;
}

TEST(threads_clock_entropy) {
    int v = 0;
    slang__plat_thread th;
    assert(slang__plat_thread_start(&th, thread_fn, &v));
    slang__plat_thread_join(th);
    assert(v == 42);
    assert(slang__plat_cpu_count() >= 1);
    int64_t a = slang__plat_mono_ns();
    struct timespec ts = { 0, 2000000 };
    nanosleep(&ts, NULL);
    assert(slang__plat_mono_ns() - a >= 2000000);
    unsigned char b1[32] = { 0 }, b2[32] = { 0 };
    assert(slang__plat_random_bytes(b1, sizeof b1));
    assert(slang__plat_random_bytes(b2, sizeof b2));
    assert(memcmp(b1, b2, sizeof b1) != 0);
    assert(slang__plat_is_ip("127.0.0.1") && slang__plat_is_ip("::1") && !slang__plat_is_ip("localhost"));
}

TEST(poller_raw) {
    slang__plat_poller *p = slang__plat_poller_new();
    assert(p);
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    slang__plat_event ev[8];
    assert(slang__plat_poller_arm(p, sv[0], SLANG_PLAT_READ, 77, false) == 0);
    assert(slang__plat_poller_wait(p, ev, 8, 10) == 0);       /* nothing to read: timeout */
    assert(write(sv[1], "x", 1) == 1);
    int n = slang__plat_poller_wait(p, ev, 8, 1000);
    assert(n == 1 && ev[0].token == 77 && (ev[0].events & SLANG_PLAT_READ));
    slang__plat_poller_wake(p);
    n = slang__plat_poller_wait(p, ev, 8, 1000);
    assert(n >= 1);
    bool woke = false;
    for (int i = 0; i < n; i++) woke |= ev[i].token == SLANG_PLAT_WAKE_TOKEN;
    assert(woke);
    slang__plat_poller_remove(p, sv[0]);
    close(sv[0]);
    close(sv[1]);
    slang__plat_poller_free(p);
}

/* ---- readiness waits through the scheduler ---- */

TEST(io_waits) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    /* readable after a write from another side */
    slang_task *t = slang_io_wait_readable(sv[0], -1);
    assert(!slang_task_is_done(t));
    assert(write(sv[1], "hi", 2) == 2);
    assert(wait_code(t) == 0);
    char buf[4];
    assert(read(sv[0], buf, sizeof buf) == 2);
    /* writable at once */
    assert(wait_code(slang_io_wait_writable(sv[0], -1)) == 0);
    /* timeout */
    int64_t t0 = now_ms();
    assert(wait_code(slang_io_wait_readable(sv[0], 30)) == 1);
    assert(now_ms() - t0 >= 29);
    /* read and write waiters on one fd at once */
    slang_task *r = slang_io_wait_readable(sv[0], -1);
    slang_task *w = slang_io_wait_writable(sv[0], -1);
    assert(wait_code(w) == 0);
    assert(!slang_task_is_done(r));
    /* forget wakes the reader with 2 */
    slang_io_forget(sv[0]);
    assert(wait_code(r) == 2);
    close(sv[0]);
    close(sv[1]);
}

TEST(io_many_fds) {
    enum { N = 40 };
    int sv[N][2];
    slang_task *ts[N];
    for (int i = 0; i < N; i++) {
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv[i]) == 0);
        ts[i] = slang_io_wait_readable(sv[i][0], 5000);
    }
    for (int i = N - 1; i >= 0; i--) assert(write(sv[i][1], "z", 1) == 1);
    slang_task *all = slang_task_all(ts, N);
    slang_task_await(all);
    slang_release(all);
    for (int i = 0; i < N; i++) {
        assert(*(int64_t *)slang_task_result(ts[i]) == 0);
        slang_release(ts[i]);
        slang_io_forget(sv[i][0]);
        close(sv[i][0]);
        close(sv[i][1]);
    }
}

/* ---- task context, signals, spin locks, yield ---- */

typedef struct ctx_probe {
    slang_task *done;    /* completed by the continuation */
    slang_object *seen;  /* context seen there (owned) */
} ctx_probe;

static void probe_cont(void *arg) {
    ctx_probe *p = arg;
    p->seen = slang_task_get_ctx();
    slang_task_complete(p->done, NULL);
}

TEST(context_follows_continuations) {
    slang_string *c1 = S("ctx-one");
    slang_task_set_ctx((slang_object *)c1);
    slang_object *got = slang_task_get_ctx();
    assert(got == (slang_object *)c1);
    slang_release(got);
    ctx_probe p = { slang_task_rt_waker(), NULL };
    slang_task *d = slang_task_delay(5);
    assert(slang_task_add_continuation(d, probe_cont, &p));
    slang_task_set_ctx(NULL);           /* the registering code's context is kept */
    slang_task_await(p.done);
    assert(p.seen == (slang_object *)c1);
    slang_release(p.seen);
    slang_release(p.done);
    slang_release(d);
    /* posted jobs inherit the poster's context too */
    slang_task_set_ctx((slang_object *)c1);
    ctx_probe q = { slang_task_rt_waker(), NULL };
    slang_sched_post(probe_cont, &q);
    slang_task_set_ctx(NULL);
    slang_task_await(q.done);
    assert(q.seen == (slang_object *)c1);
    slang_release(q.seen);
    slang_release(q.done);
    got = slang_task_get_ctx();
    assert(got == NULL);
    slang_release(c1);
}

TEST(signals_and_cancel) {
    slang_task *s = slang_task_rt_signal();
    assert(!slang_task_is_done(s));
    slang_task_rt_fire(s);
    slang_task_rt_fire(s);  /* idempotent */
    assert(slang_task_is_done(s));
    slang_release(s);
    int64_t t0 = now_ms();
    slang_task *d = slang_task_delay(60000);
    slang_task_rt_fire(d);  /* cancels the timer and completes now */
    slang_task_await(d);
    assert(now_ms() - t0 < 1000);
    slang_release(d);
    slang_task *s2 = slang_task_rt_signal();
    assert(slang_task_try_complete(s2, NULL));
    assert(!slang_task_try_complete(s2, NULL));
    slang_release(s2);
    slang_task *y = slang_task_yield();
    slang_task_await(y);
    slang_release(y);
}

static int64_t spin_slot, spin_count;

static void spin_worker(slang_task *t, void *unused) {
    (void)unused;
    for (int i = 0; i < 20000; i++) {
        slang_task_spin_lock(&spin_slot);
        spin_count++;
        slang_task_spin_unlock(&spin_slot);
    }
    slang_task_complete(t, NULL);
}

TEST(spin_lock) {
    slang_task *ts[4];
    for (int i = 0; i < 4; i++) ts[i] = slang_task_spawn(spin_worker, NULL);
    for (int i = 0; i < 4; i++) { slang_task_await(ts[i]); slang_release(ts[i]); }
    assert(spin_count == 80000 && spin_slot == 0);
}

int main(int argc, char **argv) {
    printf("test_platform\n");
    setenv("SLANG_WORKERS", "3", 1);
    int64_t live = slang_live_objects();
    slang_runtime_init(argc, argv);
    RUN(threads_clock_entropy);
    RUN(poller_raw);
    RUN(io_waits);
    RUN(io_many_fds);
    RUN(context_follows_continuations);
    RUN(signals_and_cancel);
    RUN(spin_lock);
    slang_runtime_shutdown();
    assert(slang_live_objects() == live);
    return 0;
}
