#include "test.h"
#include <pthread.h>
#include <sched.h>
#include <time.h>

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- spawn / await ---- */

static void square(slang_task *t, void *ctx) {
    int64_t v = (int64_t)(intptr_t)ctx;
    v *= v;
    slang_task_complete(t, &v);
}

TEST(spawn_await) {
    slang_task *t = slang_task_spawn(square, (void *)(intptr_t)12);
    int64_t *r = slang_task_await(t);
    assert(*r == 144);
    assert(slang_task_is_done(t));
    assert(*(int64_t *)slang_task_result(t) == 144);
    slang_release(t);
}

static int64_t counter;
static void bump(slang_task *t, void *ctx) {
    (void)ctx;
    __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
    slang_task_complete(t, NULL);
}

TEST(many_spawns_all) {
    enum { N = 2000 };
    static slang_task *ts[N];
    for (int i = 0; i < N; i++) ts[i] = slang_task_spawn(bump, NULL);
    slang_task *all = slang_task_all(ts, N);
    slang_task_await(all);
    assert(counter == N);
    for (int i = 0; i < N; i++) { assert(slang_task_is_done(ts[i])); slang_release(ts[i]); }
    slang_release(all);
    slang_task *empty = slang_task_all(NULL, 0);
    assert(slang_task_is_done(empty));
    slang_release(empty);
}

/* ---- nested blocking await inside a worker (help mode, no deadlock) ---- */

static void outer(slang_task *t, void *ctx) {
    (void)ctx;
    int64_t sum = 0;
    slang_task *inner[8];
    for (int i = 0; i < 8; i++) inner[i] = slang_task_spawn(square, (void *)(intptr_t)i);
    for (int i = 0; i < 8; i++) { sum += *(int64_t *)slang_task_await(inner[i]); slang_release(inner[i]); }
    slang_task_complete(t, &sum);
}

TEST(nested_await_on_worker) {
    slang_task *ts[64];
    for (int i = 0; i < 64; i++) ts[i] = slang_task_spawn(outer, NULL);
    for (int i = 0; i < 64; i++) {
        assert(*(int64_t *)slang_task_await(ts[i]) == 140);
        slang_release(ts[i]);
    }
}

/* ---- hand-lowered coroutine using the switched-resume frame layout ---- */

typedef struct frame {
    void (*resume)(void *);   /* +0: what llvm.coro.resume calls */
    void (*destroy)(void *);  /* +8 */
    int state;
    slang_task *self;
    slang_task *awaited;
    int64_t arg, acc;
} frame;

static void co_body(void *p) {
    frame *f = p;
    switch (f->state) {
    case 0:
        f->awaited = slang_task_delay(5 + f->arg % 7);
        f->state = 1;
        if (slang_task_add_continuation(f->awaited, slang_coro_resume, f)) return; /* suspend */
        __attribute__((fallthrough)); /* already done: continue without suspending */
    case 1:
        slang_release(f->awaited);
        f->awaited = slang_task_spawn(square, (void *)(intptr_t)f->arg);
        f->state = 2;
        if (slang_task_add_continuation(f->awaited, slang_coro_resume, f)) return;
        __attribute__((fallthrough));
    case 2:
        f->acc = *(int64_t *)slang_task_result(f->awaited) + 1;
        slang_release(f->awaited);
        slang_task_complete(f->self, &f->acc);  /* return value */
        slang_release(f->self);                 /* frame's reference */
        slang_free(f);                          /* final suspend + coro.free */
        return;
    }
}

/* Ramp function of `async func f(arg int) int`. */
static slang_task *async_f(int64_t arg) {
    frame *f = slang_alloc(sizeof *f);
    f->resume = co_body;
    f->self = slang_task_new(8, NULL);
    slang_retain(f->self);        /* +1 for the frame, +1 returned to caller */
    f->arg = arg;
    slang_task *t = f->self;
    co_body(f);                   /* run synchronously until the first suspend */
    return t;
}

TEST(coroutine_protocol) {
    enum { N = 200 };
    slang_task *ts[N];
    for (int i = 0; i < N; i++) ts[i] = async_f(i);
    slang_task *all = slang_task_all(ts, N);
    slang_task_await(all);
    slang_release(all);
    for (int i = 0; i < N; i++) {
        assert(*(int64_t *)slang_task_result(ts[i]) == (int64_t)i * i + 1);
        slang_release(ts[i]);
    }
}

/* ---- delay / any ---- */

TEST(delay_any) {
    int64_t t0 = now_ms();
    slang_task *d = slang_task_delay(50);
    slang_task_await(d);
    assert(now_ms() - t0 >= 50);
    slang_release(d);
    slang_task *z = slang_task_delay(0);
    assert(slang_task_is_done(z));
    slang_release(z);
    slang_task *ds[3] = { slang_task_delay(300), slang_task_delay(10), slang_task_delay(200) };
    slang_task *any = slang_task_any(ds, 3);
    assert(*(int64_t *)slang_task_await(any) == 1);
    slang_release(any);
    slang_task *all = slang_task_all(ds, 3);
    slang_task_await(all);
    slang_release(all);
    for (int i = 0; i < 3; i++) slang_release(ds[i]);
}

/* ---- results with refcounted payloads ---- */

static void make_str(slang_task *t, void *ctx) {
    (void)ctx;
    slang_string *s = S("result");
    slang_task_complete(t, &s);   /* task retains */
    slang_release(s);
}

TEST(refcounted_result) {
    slang_task *t = slang_task_spawn_ex(make_str, NULL, sizeof(slang_string *), &slang_ops_ref);
    slang_string *s = *(slang_string **)slang_task_await(t);
    assert(STR_IS(s, "result"));
    slang_retain(s);              /* take our own copy before the task dies */
    slang_release(t);
    /* the worker may still be inside make_str or hold its job reference to t, so the
     * exact count is 1..3 here; leaks are checked after shutdown in main() */
    assert(STR_IS(s, "result") && slang_refcount(s) >= 1 && slang_refcount(s) <= 3);
    slang_release(s);
}

/* ---- mutex ---- */

static slang_mutex *mu;
static int64_t guarded;

static void locker(slang_task *t, void *ctx) {
    (void)ctx;
    for (int i = 0; i < 2000; i++) {
        slang_mutex_lock(mu);
        int64_t v = guarded;
        guarded = v + 1;
        slang_mutex_unlock(mu);
    }
    slang_task_complete(t, NULL);
}

TEST(mutex) {
    mu = slang_mutex_new();
    assert(slang_mutex_try_lock(mu));
    assert(!slang_mutex_try_lock(mu));
    slang_task *pending = slang_mutex_lock_async(mu);
    assert(!slang_task_is_done(pending));
    slang_mutex_unlock(mu);                 /* hands ownership to `pending` */
    slang_task_await(pending);
    assert(!slang_mutex_try_lock(mu));
    slang_mutex_unlock(mu);
    slang_release(pending);

    slang_task *ts[8];
    for (int i = 0; i < 8; i++) ts[i] = slang_task_spawn(locker, NULL);
    for (int i = 0; i < 8; i++) { slang_task_await(ts[i]); slang_release(ts[i]); }
    assert(guarded == 8 * 2000);
    slang_release(mu);
}

/* ---- channels ---- */

typedef struct { slang_chan *ch; int64_t n; } prod_ctx;

static void producer(slang_task *t, void *ctx) {
    prod_ctx *pc = ctx;
    for (int64_t i = 1; i <= pc->n; i++) slang_chan_send(pc->ch, &i);
    slang_chan_close(pc->ch);
    slang_task_complete(t, NULL);
}

static void chan_roundtrip(int64_t cap) {
    slang_chan *ch = slang_chan_new(8, NULL, cap);
    prod_ctx pc = { ch, 5000 };
    slang_task *p = slang_task_spawn(producer, &pc);
    int64_t v, sum = 0, n = 0;
    while (slang_chan_recv(ch, &v)) { sum += v; n++; }
    assert(n == 5000 && sum == 5000 * 5001 / 2);
    assert(!slang_chan_recv(ch, &v) && v == 0);   /* closed: zero value, ok=false */
    slang_task_await(p);
    slang_release(p);
    slang_release(ch);
}

TEST(channel_unbuffered) { chan_roundtrip(0); }
TEST(channel_buffered) { chan_roundtrip(16); }

TEST(channel_async_and_strings) {
    slang_chan *ch = slang_chan_new(sizeof(slang_string *), &slang_ops_ref, 2);
    slang_task *r1 = slang_chan_recv_async(ch);         /* waits */
    assert(!slang_task_is_done(r1));
    slang_string *a = S("a"), *b = S("b"), *c = S("c");
    slang_task *s1 = slang_chan_send_async(ch, &a);      /* hands off to r1 */
    assert(slang_task_is_done(s1) && slang_task_is_done(r1));
    unsigned char *res = slang_task_result(r1);
    assert(*(int64_t *)res == 1 && *(slang_string **)(res + 8) == a);
    slang_task *s2 = slang_chan_send_async(ch, &b);      /* buffered */
    slang_task *s3 = slang_chan_send_async(ch, &c);      /* buffered */
    slang_task *s4 = slang_chan_send_async(ch, &a);      /* blocks: buffer full */
    assert(slang_task_is_done(s2) && slang_task_is_done(s3) && !slang_task_is_done(s4));
    assert(slang_chan_len(ch) == 2);
    slang_string *got;
    assert(slang_chan_recv(ch, &got) && got == b);       /* refills from s4 */
    slang_release(got);
    assert(slang_task_is_done(s4));
    slang_release(s1); slang_release(s2); slang_release(s3); slang_release(s4);
    slang_release(r1);
    slang_release(ch);                                   /* drops buffered c, a */
    assert(slang_refcount(a) == 1 && slang_refcount(b) == 1 && slang_refcount(c) == 1);
    slang_release(a); slang_release(b); slang_release(c);
}

/* ---- misuse ---- */

static void double_complete(void) {
    slang_task *t = slang_task_new(0, NULL);
    slang_task_complete(t, NULL);
    slang_task_complete(t, NULL);
}

static void early_result(void) {
    slang_task *t = slang_task_new(8, NULL);
    slang_task_result(t);
}

/* ---- a destructor run by a worker's release of the job's task starts a
 * task (regression: the worker's current-task slot still named the dying
 * task, which the new task took as its parent: resurrected, double free) ---- */

static int64_t drop_job_go, drop_ran;

static void guard_drop(void *obj) {
    (void)obj;
    slang_task *n = slang_task_new(0, NULL);   /* parent = the current task */
    slang_task_complete(n, NULL);
    slang_release(n);
    __atomic_store_n(&drop_ran, 1, __ATOMIC_RELEASE);
}

static const slang_type_info guard_type = { "Guard", 0, guard_drop };

static void wait_flag(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&drop_job_go, __ATOMIC_ACQUIRE)) sched_yield();
}

TEST(drop_in_job_teardown_starts_task) {
    for (int round = 0; round < 50; round++) {
        drop_job_go = drop_ran = 0;
        slang_task *t = slang_task_new_plain(8, &slang_ops_ref);
        void *g = slang_object_new(&guard_type, 16);
        slang_task_complete(t, &g);              /* the result holds the guard */
        slang_release(g);
        slang_task *prev = slang_task_enter_scope(t);
        slang_sched_post(wait_flag, NULL);       /* the job runs as task t */
        slang_task_leave_scope(prev);
        slang_release(prev);
        slang_release(t);                        /* the job's is the last reference */
        __atomic_store_n(&drop_job_go, 1, __ATOMIC_RELEASE);
        int64_t t0 = now_ms();
        while (!__atomic_load_n(&drop_ran, __ATOMIC_ACQUIRE)) {
            assert(now_ms() - t0 < 5000);
            sched_yield();
        }
    }
}

TEST(misuse_panics) {
    char buf[8192];
    assert(run_child(double_complete, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: task completed twice"));
    assert(run_child(early_result, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: task result read before completion"));
}

int main(int argc, char **argv) {
    printf("test_task\n");
    setenv("SLANG_WORKERS", "3", 1);
    int64_t live = slang_live_objects();
    slang_runtime_init(argc, argv);
    assert(slang_sched_workers() == 3);
    assert(!slang_sched_is_worker());
    slang_slice args;
    slang_os_args(&args);
    assert(args.len == argc);
    assert(strstr(slang_string_data(*(slang_string **)slang_slice_index(&args, 0, 8)), "test_task"));
    slang_slice_release(&args);

    RUN(misuse_panics);   /* forks: run before worker threads are busy */
    RUN(spawn_await);
    RUN(many_spawns_all);
    RUN(nested_await_on_worker);
    RUN(coroutine_protocol);
    RUN(delay_any);
    RUN(refcounted_result);
    RUN(mutex);
    RUN(channel_unbuffered);
    RUN(channel_buffered);
    RUN(channel_async_and_strings);
    RUN(drop_in_job_teardown_starts_task);

    slang_runtime_shutdown();
    assert(slang_live_objects() == live);
    return 0;
}
