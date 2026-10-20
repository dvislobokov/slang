/* task.c - thread-pool scheduler, tasks, timers, mutex, channels. */
#include "rt_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================
 * Scheduler: N workers over one global FIFO queue.
 * ====================================================================== */

typedef struct job {
    slang_resume_fn fn;
    void *arg;
    struct job *next;
} job;

static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;     /* work available / stop */
static pthread_cond_t idle_cv = PTHREAD_COND_INITIALIZER;  /* drained / thread exited */
static job *q_head, *q_tail;
static int q_stopping;
static int q_started;
static int64_t q_active;      /* jobs currently running */
static int64_t q_nworkers;    /* target parallelism N */
static int64_t q_alive;       /* worker threads alive (N core + spares) */
static int64_t q_idle;        /* workers parked waiting for work */
static int64_t q_blocked;     /* workers blocked inside slang_task_await */

static _Thread_local int t_is_worker;

static job *pop_locked(void) {
    job *j = q_head;
    if (j) {
        q_head = j->next;
        if (!q_head) q_tail = NULL;
    }
    return j;
}

static void *worker_main(void *spare_flag) {
    int spare = spare_flag != NULL;
    t_is_worker = 1;
    pthread_mutex_lock(&q_mu);
    for (;;) {
        job *j = pop_locked();
        if (!j) {
            if (q_stopping) break;
            /* spare threads retire once enough unblocked workers exist again */
            if (spare && q_alive - q_blocked > q_nworkers) break;
            q_idle++;
            pthread_cond_wait(&q_cv, &q_mu);
            q_idle--;
            continue;
        }
        q_active++;
        pthread_mutex_unlock(&q_mu);
        slang_resume_fn fn = j->fn;
        void *arg = j->arg;
        free(j);
        fn(arg);
        pthread_mutex_lock(&q_mu);
        q_active--;
        if (!q_head && q_active == 0) pthread_cond_broadcast(&idle_cv);
    }
    q_alive--;
    pthread_cond_broadcast(&idle_cv);
    pthread_mutex_unlock(&q_mu);
    return NULL;
}

/* q_mu must be held. */
static void spawn_worker_locked(int spare) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, worker_main, spare ? (void *)1 : NULL) != 0)
        slang_panic("cannot create worker thread");
    pthread_attr_destroy(&attr);
    q_alive++;
}

static int64_t worker_count(void) {
    const char *env = getenv("SLANG_WORKERS");
    long n = env ? strtol(env, NULL, 10) : 0;
    if (n <= 0) n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 1;
    if (n > 256) n = 256;
    return n;
}

static void timers_start(void);
static void timers_stop(void);

void slang__sched_start(void) {
    pthread_mutex_lock(&q_mu);
    if (q_started) { pthread_mutex_unlock(&q_mu); return; }
    q_stopping = 0;
    q_nworkers = worker_count();
    for (int64_t i = 0; i < q_nworkers; i++) spawn_worker_locked(0);
    __atomic_store_n(&q_started, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&q_mu);
    timers_start();
}

void slang__sched_stop(void) {
    pthread_mutex_lock(&q_mu);
    if (!q_started) { pthread_mutex_unlock(&q_mu); return; }
    while (q_head || q_active > 0) pthread_cond_wait(&idle_cv, &q_mu);  /* drain */
    q_stopping = 1;
    pthread_cond_broadcast(&q_cv);
    while (q_alive > 0) pthread_cond_wait(&idle_cv, &q_mu);
    pthread_mutex_unlock(&q_mu);
    timers_stop();
    pthread_mutex_lock(&q_mu);
    q_nworkers = 0;
    __atomic_store_n(&q_started, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&q_mu);
}

void slang_sched_post(slang_resume_fn fn, void *arg) {
    if (!fn) slang_panic_nil();
    if (!__atomic_load_n(&q_started, __ATOMIC_ACQUIRE)) slang__sched_start();
    job *j = malloc(sizeof *j);
    if (!j) slang_panic("out of memory (job)");
    j->fn = fn;
    j->arg = arg;
    j->next = NULL;
    pthread_mutex_lock(&q_mu);
    if (q_tail) q_tail->next = j; else q_head = j;
    q_tail = j;
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_mu);
}

int64_t slang_sched_workers(void) {
    if (!__atomic_load_n(&q_started, __ATOMIC_ACQUIRE)) slang__sched_start();
    return q_nworkers;
}

bool slang_sched_is_worker(void) { return t_is_worker != 0; }

void slang_coro_resume(void *frame) {
    if (!frame) slang_panic_nil();
    /* LLVM switched-resume ABI: the first word of a coroutine frame is the resume fn. */
    void (*resume)(void *) = *(void (**)(void *))frame;
    resume(frame);
}

/* ======================================================================
 * Tasks
 * ====================================================================== */

enum { TASK_PENDING = 0, TASK_DONE = 1 };

typedef struct cont {
    slang_resume_fn fn;
    void *frame;
    struct cont *next;
} cont;

struct slang_task {
    slang_object hdr;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int32_t state;              /* atomic */
    int32_t _pad;
    int64_t result_size;
    const slang_value_ops *ops; /* applied at slot + ops_offset */
    int64_t ops_offset;
    cont *conts;                /* LIFO; reversed when fired */
    unsigned char slot[];       /* result_size bytes (8-aligned) */
};

static void task_drop(void *obj) {
    slang_task *t = obj;
    if (t->state == TASK_DONE && t->ops && t->ops->release)
        t->ops->release(t->slot + t->ops_offset);
    cont *c = t->conts;
    while (c) { cont *n = c->next; free(c); c = n; }
    pthread_mutex_destroy(&t->mu);
    pthread_cond_destroy(&t->cv);
}

static const slang_type_info task_type = { "Task", 0, task_drop };

static slang_task *task_new_ex(int64_t result_size, const slang_value_ops *ops, int64_t ops_off) {
    if (result_size < 0) slang__panicf("slang_task_new: negative result size");
    slang_task *t = slang_object_new(&task_type,
                                     (int64_t)sizeof(slang_task) + SLANG_ROUND8(result_size));
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->result_size = result_size;
    t->ops = ops;
    t->ops_offset = ops_off;
    return t;
}

slang_task *slang_task_new(int64_t result_size, const slang_value_ops *result_ops) {
    return task_new_ex(result_size, result_ops, 0);
}

void slang_task_complete(slang_task *t, const void *result) {
    if (!t) slang_panic_nil();
    pthread_mutex_lock(&t->mu);
    if (t->state == TASK_DONE) {
        pthread_mutex_unlock(&t->mu);
        slang_panic("task completed twice");
    }
    if (result && t->result_size > 0) {
        memcpy(t->slot, result, (size_t)t->result_size);
        if (t->ops && t->ops->retain) t->ops->retain(t->slot + t->ops_offset);
    }
    __atomic_store_n(&t->state, TASK_DONE, __ATOMIC_RELEASE);
    cont *c = t->conts, *rev = NULL;
    t->conts = NULL;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    /* After this point `t` may be freed by another thread; use only `rev`. */
    while (c) { cont *n = c->next; c->next = rev; rev = c; c = n; }
    while (rev) {
        cont *n = rev->next;
        slang_sched_post(rev->fn, rev->frame);
        free(rev);
        rev = n;
    }
}

bool slang_task_is_done(const slang_task *t) {
    if (!t) slang_panic_nil();
    return __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_DONE;
}

bool slang_task_add_continuation(slang_task *t, slang_resume_fn fn, void *frame) {
    if (!t || !fn) slang_panic_nil();
    if (slang_task_is_done(t)) return false;
    cont *c = malloc(sizeof *c);
    if (!c) slang_panic("out of memory (continuation)");
    c->fn = fn;
    c->frame = frame;
    pthread_mutex_lock(&t->mu);
    if (t->state == TASK_DONE) {
        pthread_mutex_unlock(&t->mu);
        free(c);
        return false;
    }
    c->next = t->conts;
    t->conts = c;
    pthread_mutex_unlock(&t->mu);
    return true;
}

void *slang_task_result(const slang_task *t) {
    if (!slang_task_is_done(t)) slang_panic("task result read before completion");
    return (void *)t->slot;
}

void *slang_task_await(slang_task *t) {
    if (!t) slang_panic_nil();
    if (slang_task_is_done(t)) return t->slot;
    if (t_is_worker) {
        /* Blocking a worker: make sure the pool keeps N runnable threads by
         * starting a spare worker if nobody is idle (it retires later). */
        pthread_mutex_lock(&q_mu);
        q_blocked++;
        if (q_idle == 0 && !q_stopping) spawn_worker_locked(1);
        pthread_mutex_unlock(&q_mu);
    }
    pthread_mutex_lock(&t->mu);
    while (t->state != TASK_DONE) pthread_cond_wait(&t->cv, &t->mu);
    pthread_mutex_unlock(&t->mu);
    if (t_is_worker) {
        pthread_mutex_lock(&q_mu);
        q_blocked--;
        pthread_mutex_unlock(&q_mu);
    }
    return t->slot;
}

typedef struct spawn_ctx {
    slang_task_fn fn;
    slang_task *task;
    void *ctx;
} spawn_ctx;

static void spawn_tramp(void *arg) {
    spawn_ctx *sc = arg;
    slang_task_fn fn = sc->fn;
    slang_task *t = sc->task;
    void *ctx = sc->ctx;
    free(sc);
    fn(t, ctx);
    slang_release(t);  /* the job's reference */
}

slang_task *slang_task_spawn_ex(slang_task_fn fn, void *ctx, int64_t result_size,
                                const slang_value_ops *result_ops) {
    if (!fn) slang_panic_nil();
    slang_task *t = slang_task_new(result_size, result_ops);
    spawn_ctx *sc = malloc(sizeof *sc);
    if (!sc) slang_panic("out of memory (spawn)");
    sc->fn = fn;
    sc->task = t;
    sc->ctx = ctx;
    slang_retain(t);  /* held by the job until fn returns */
    slang_sched_post(spawn_tramp, sc);
    return t;
}

slang_task *slang_task_spawn(slang_task_fn fn, void *ctx) {
    return slang_task_spawn_ex(fn, ctx, 8, NULL);
}

/* ======================================================================
 * Timers (slang_task_delay): one thread, sorted list, monotonic clock.
 * ====================================================================== */

typedef struct timer {
    struct timespec deadline;
    slang_task *task;   /* owned */
    struct timer *next;
} timer;

static pthread_mutex_t tm_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tm_cv;
static timer *tm_list;
static int tm_running, tm_stop;
static pthread_t tm_thread;

static int ts_cmp(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec ? -1 : 1;
    if (a->tv_nsec != b->tv_nsec) return a->tv_nsec < b->tv_nsec ? -1 : 1;
    return 0;
}

static void *timer_main(void *unused) {
    (void)unused;
    pthread_mutex_lock(&tm_mu);
    while (!tm_stop) {
        if (!tm_list) { pthread_cond_wait(&tm_cv, &tm_mu); continue; }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ts_cmp(&tm_list->deadline, &now) > 0) {
            pthread_cond_timedwait(&tm_cv, &tm_mu, &tm_list->deadline);
            continue;
        }
        timer *t = tm_list;
        tm_list = t->next;
        pthread_mutex_unlock(&tm_mu);
        slang_task_complete(t->task, NULL);
        slang_release(t->task);
        free(t);
        pthread_mutex_lock(&tm_mu);
    }
    /* Pending timers at shutdown are dropped without completing. */
    while (tm_list) {
        timer *t = tm_list;
        tm_list = t->next;
        slang_release(t->task);
        free(t);
    }
    pthread_mutex_unlock(&tm_mu);
    return NULL;
}

static void timers_start(void) {
    pthread_mutex_lock(&tm_mu);
    if (!tm_running) {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&tm_cv, &ca);
        pthread_condattr_destroy(&ca);
        tm_stop = 0;
        if (pthread_create(&tm_thread, NULL, timer_main, NULL) != 0)
            slang_panic("cannot create timer thread");
        tm_running = 1;
    }
    pthread_mutex_unlock(&tm_mu);
}

static void timers_stop(void) {
    pthread_mutex_lock(&tm_mu);
    if (!tm_running) { pthread_mutex_unlock(&tm_mu); return; }
    tm_stop = 1;
    pthread_cond_signal(&tm_cv);
    pthread_mutex_unlock(&tm_mu);
    pthread_join(tm_thread, NULL);
    pthread_mutex_lock(&tm_mu);
    tm_running = 0;
    pthread_cond_destroy(&tm_cv);
    pthread_mutex_unlock(&tm_mu);
}

slang_task *slang_task_delay(int64_t ms) {
    slang_task *t = slang_task_new(0, NULL);
    if (ms <= 0) { slang_task_complete(t, NULL); return t; }
    if (!__atomic_load_n(&q_started, __ATOMIC_ACQUIRE)) slang__sched_start();
    timer *tm = malloc(sizeof *tm);
    if (!tm) slang_panic("out of memory (timer)");
    clock_gettime(CLOCK_MONOTONIC, &tm->deadline);
    tm->deadline.tv_sec += ms / 1000;
    tm->deadline.tv_nsec += (ms % 1000) * 1000000L;
    if (tm->deadline.tv_nsec >= 1000000000L) {
        tm->deadline.tv_sec++;
        tm->deadline.tv_nsec -= 1000000000L;
    }
    slang_retain(t);
    tm->task = t;
    pthread_mutex_lock(&tm_mu);
    timer **pp = &tm_list;
    while (*pp && ts_cmp(&(*pp)->deadline, &tm->deadline) <= 0) pp = &(*pp)->next;
    tm->next = *pp;
    *pp = tm;
    pthread_cond_signal(&tm_cv);
    pthread_mutex_unlock(&tm_mu);
    return t;
}

/* ======================================================================
 * Combinators
 * ====================================================================== */

typedef struct all_state {
    slang_task *result;    /* owned */
    int64_t remaining;     /* atomic */
} all_state;

static void all_one_done(void *arg) {
    all_state *st = arg;
    if (__atomic_sub_fetch(&st->remaining, 1, __ATOMIC_ACQ_REL) == 0) {
        slang_task_complete(st->result, NULL);
        slang_release(st->result);
        free(st);
    }
}

slang_task *slang_task_all(slang_task *const *tasks, int64_t n) {
    slang_task *r = slang_task_new(0, NULL);
    all_state *st = malloc(sizeof *st);
    if (!st) slang_panic("out of memory (task_all)");
    slang_retain(r);
    st->result = r;
    st->remaining = n + 1;  /* +1 guard released below */
    for (int64_t i = 0; i < n; i++) {
        if (!tasks[i]) slang_panic_nil();
        if (!slang_task_add_continuation(tasks[i], all_one_done, st)) all_one_done(st);
    }
    all_one_done(st);
    return r;
}

typedef struct any_state {
    slang_task *result;    /* owned */
    int64_t refs;          /* atomic: outstanding callbacks + guard */
    int32_t fired;         /* atomic */
} any_state;

typedef struct any_node {
    any_state *st;
    int64_t index;
} any_node;

static void any_unref(any_state *st) {
    if (__atomic_sub_fetch(&st->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        slang_release(st->result);
        free(st);
    }
}

static void any_one_done(void *arg) {
    any_node *nd = arg;
    any_state *st = nd->st;
    int64_t idx = nd->index;
    free(nd);
    if (__atomic_exchange_n(&st->fired, 1, __ATOMIC_ACQ_REL) == 0)
        slang_task_complete(st->result, &idx);
    any_unref(st);
}

slang_task *slang_task_any(slang_task *const *tasks, int64_t n) {
    if (n <= 0) slang_panic("task.Any called with no tasks");
    slang_task *r = slang_task_new(8, NULL);
    any_state *st = malloc(sizeof *st);
    if (!st) slang_panic("out of memory (task_any)");
    slang_retain(r);
    st->result = r;
    st->refs = n + 1;
    st->fired = 0;
    for (int64_t i = 0; i < n; i++) {
        if (!tasks[i]) slang_panic_nil();
        any_node *nd = malloc(sizeof *nd);
        if (!nd) slang_panic("out of memory (task_any)");
        nd->st = st;
        nd->index = i;
        if (!slang_task_add_continuation(tasks[i], any_one_done, nd)) any_one_done(nd);
    }
    any_unref(st);
    return r;
}

/* ======================================================================
 * Mutex (async-aware: waiters are tasks, unlock hands ownership over)
 * ====================================================================== */

typedef struct waiter {
    slang_task *task;      /* owned */
    struct waiter *next;
    unsigned char elem[];  /* channel senders: owned element copy */
} waiter;

typedef struct wqueue { waiter *head, *tail; } wqueue;

static void wq_push(wqueue *q, waiter *w) {
    w->next = NULL;
    if (q->tail) q->tail->next = w; else q->head = w;
    q->tail = w;
}

static waiter *wq_pop(wqueue *q) {
    waiter *w = q->head;
    if (w) { q->head = w->next; if (!q->head) q->tail = NULL; }
    return w;
}

struct slang_mutex {
    slang_object hdr;
    pthread_mutex_t mu;
    int32_t locked;
    wqueue waiters;
};

static void mutex_drop(void *obj) {
    slang_mutex *m = obj;
    waiter *w;
    while ((w = wq_pop(&m->waiters))) { slang_release(w->task); free(w); }
    pthread_mutex_destroy(&m->mu);
}

static const slang_type_info mutex_type = { "Mutex", (int64_t)sizeof(struct slang_mutex), mutex_drop };

slang_mutex *slang_mutex_new(void) {
    slang_mutex *m = slang_object_new(&mutex_type, (int64_t)sizeof(struct slang_mutex));
    pthread_mutex_init(&m->mu, NULL);
    return m;
}

slang_task *slang_mutex_lock_async(slang_mutex *m) {
    if (!m) slang_panic_nil();
    slang_task *t = slang_task_new(0, NULL);
    pthread_mutex_lock(&m->mu);
    if (!m->locked) {
        m->locked = 1;
        pthread_mutex_unlock(&m->mu);
        slang_task_complete(t, NULL);
        return t;
    }
    waiter *w = malloc(sizeof *w);
    if (!w) slang_panic("out of memory (mutex)");
    slang_retain(t);
    w->task = t;
    wq_push(&m->waiters, w);
    pthread_mutex_unlock(&m->mu);
    return t;
}

void slang_mutex_lock(slang_mutex *m) {
    slang_task *t = slang_mutex_lock_async(m);
    slang_task_await(t);
    slang_release(t);
}

bool slang_mutex_try_lock(slang_mutex *m) {
    if (!m) slang_panic_nil();
    pthread_mutex_lock(&m->mu);
    bool ok = !m->locked;
    if (ok) m->locked = 1;
    pthread_mutex_unlock(&m->mu);
    return ok;
}

void slang_mutex_unlock(slang_mutex *m) {
    if (!m) slang_panic_nil();
    pthread_mutex_lock(&m->mu);
    if (!m->locked) { pthread_mutex_unlock(&m->mu); slang_panic("unlock of unlocked mutex"); }
    waiter *w = wq_pop(&m->waiters);
    if (!w) m->locked = 0;           /* else: ownership passes directly to w */
    pthread_mutex_unlock(&m->mu);
    if (w) {
        slang_task_complete(w->task, NULL);
        slang_release(w->task);
        free(w);
    }
}

/* ======================================================================
 * Channels
 * ====================================================================== */

struct slang_chan {
    slang_object hdr;
    pthread_mutex_t mu;
    int64_t elem_size;
    const slang_value_ops *ops;
    int64_t capacity;
    int64_t count, head;     /* ring buffer of `capacity` elements */
    unsigned char *buf;
    int32_t closed;
    wqueue senders;          /* waiter.elem holds the (owned) value */
    wqueue receivers;
};

static void val_retain(slang_chan *c, void *v) { if (c->ops && c->ops->retain) c->ops->retain(v); }
static void val_release(slang_chan *c, void *v) { if (c->ops && c->ops->release) c->ops->release(v); }

static void chan_drop(void *obj) {
    slang_chan *c = obj;
    for (int64_t i = 0; i < c->count; i++)
        val_release(c, c->buf + ((c->head + i) % c->capacity) * c->elem_size);
    waiter *w;
    while ((w = wq_pop(&c->senders))) { val_release(c, w->elem); slang_release(w->task); free(w); }
    while ((w = wq_pop(&c->receivers))) { slang_release(w->task); free(w); }
    slang_free(c->buf);
    pthread_mutex_destroy(&c->mu);
}

static const slang_type_info chan_type = { "Channel", (int64_t)sizeof(struct slang_chan), chan_drop };

slang_chan *slang_chan_new(int64_t elem_size, const slang_value_ops *ops, int64_t capacity) {
    if (elem_size < 0 || capacity < 0) slang_panic("slang_chan_new: invalid size");
    slang_chan *c = slang_object_new(&chan_type, (int64_t)sizeof(struct slang_chan));
    pthread_mutex_init(&c->mu, NULL);
    c->elem_size = elem_size;
    c->ops = ops;
    c->capacity = capacity;
    c->buf = capacity ? slang_alloc(capacity * (elem_size ? elem_size : 1)) : NULL;
    return c;
}

int64_t slang_chan_len(const slang_chan *c) {
    if (!c) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&c->mu);
    int64_t n = c->count;
    pthread_mutex_unlock((pthread_mutex_t *)&c->mu);
    return n;
}

/* Complete a receive task with {ok, value}. value is borrowed (task retains). */
static void finish_recv(slang_chan *c, slang_task *t, int64_t ok, const void *value) {
    unsigned char tmp[8 + 256];
    int64_t sz = 8 + c->elem_size;
    unsigned char *r = (size_t)sz <= sizeof tmp ? tmp : slang_alloc(sz);
    memset(r, 0, (size_t)sz);
    *(int64_t *)r = ok;
    if (value) memcpy(r + 8, value, (size_t)c->elem_size);
    slang_task_complete(t, r);
    if (r != tmp) slang_free(r);
}

static slang_task *recv_task_new(slang_chan *c) {
    return task_new_ex(8 + c->elem_size, c->ops, 8);
}

slang_task *slang_chan_send_async(slang_chan *c, const void *elem) {
    if (!c) slang_panic_nil();
    slang_task *done = slang_task_new(0, NULL);
    waiter *sw = malloc(sizeof(waiter) + (size_t)c->elem_size);  /* holds owned copy */
    if (!sw) slang_panic("out of memory (channel)");
    memcpy(sw->elem, elem, (size_t)c->elem_size);
    val_retain(c, sw->elem);

    pthread_mutex_lock(&c->mu);
    if (c->closed) {
        pthread_mutex_unlock(&c->mu);
        slang_panic("send on closed channel");
    }
    waiter *rw = wq_pop(&c->receivers);
    if (rw) {
        pthread_mutex_unlock(&c->mu);
        finish_recv(c, rw->task, 1, sw->elem);
        slang_release(rw->task);
        free(rw);
        val_release(c, sw->elem);
        free(sw);
        slang_task_complete(done, NULL);
        return done;
    }
    if (c->count < c->capacity) {
        memcpy(c->buf + ((c->head + c->count) % c->capacity) * c->elem_size, sw->elem,
               (size_t)c->elem_size);  /* ownership moves into the buffer */
        c->count++;
        pthread_mutex_unlock(&c->mu);
        free(sw);
        slang_task_complete(done, NULL);
        return done;
    }
    slang_retain(done);
    sw->task = done;
    wq_push(&c->senders, sw);
    pthread_mutex_unlock(&c->mu);
    return done;
}

slang_task *slang_chan_recv_async(slang_chan *c) {
    if (!c) slang_panic_nil();
    slang_task *t = recv_task_new(c);
    unsigned char tmp[256];
    unsigned char *v = (size_t)c->elem_size <= sizeof tmp ? tmp : slang_alloc(c->elem_size);
    waiter *sender = NULL;

    pthread_mutex_lock(&c->mu);
    if (c->count > 0) {
        unsigned char *slot = c->buf + c->head * c->elem_size;
        memcpy(v, slot, (size_t)c->elem_size);   /* take ownership */
        c->head = (c->head + 1) % c->capacity;
        c->count--;
        sender = wq_pop(&c->senders);
        if (sender) {   /* refill the buffer from a blocked sender */
            memcpy(c->buf + ((c->head + c->count) % c->capacity) * c->elem_size, sender->elem,
                   (size_t)c->elem_size);
            c->count++;
        }
    } else if ((sender = wq_pop(&c->senders))) {
        memcpy(v, sender->elem, (size_t)c->elem_size);  /* direct hand-off (unbuffered) */
    } else if (c->closed) {
        pthread_mutex_unlock(&c->mu);
        finish_recv(c, t, 0, NULL);
        if (v != tmp) slang_free(v);
        return t;
    } else {
        waiter *w = malloc(sizeof *w);
        if (!w) slang_panic("out of memory (channel)");
        slang_retain(t);
        w->task = t;
        wq_push(&c->receivers, w);
        pthread_mutex_unlock(&c->mu);
        if (v != tmp) slang_free(v);
        return t;
    }
    pthread_mutex_unlock(&c->mu);
    finish_recv(c, t, 1, v);
    val_release(c, v);
    if (v != tmp) slang_free(v);
    if (sender) {
        slang_task_complete(sender->task, NULL);
        slang_release(sender->task);
        free(sender);
    }
    return t;
}

void slang_chan_send(slang_chan *c, const void *elem) {
    slang_task *t = slang_chan_send_async(c, elem);
    slang_task_await(t);
    slang_release(t);
}

bool slang_chan_recv(slang_chan *c, void *out) {
    slang_task *t = slang_chan_recv_async(c);
    unsigned char *r = slang_task_await(t);
    bool ok = *(int64_t *)r != 0;
    if (out) {
        memcpy(out, r + 8, (size_t)c->elem_size);
        val_retain(c, out);
    }
    slang_release(t);
    return ok;
}

void slang_chan_close(slang_chan *c) {
    if (!c) slang_panic_nil();
    pthread_mutex_lock(&c->mu);
    if (c->closed) { pthread_mutex_unlock(&c->mu); slang_panic("close of closed channel"); }
    if (c->senders.head) { pthread_mutex_unlock(&c->mu); slang_panic("send on closed channel"); }
    c->closed = 1;
    wqueue rs = c->receivers;
    c->receivers.head = c->receivers.tail = NULL;
    pthread_mutex_unlock(&c->mu);
    waiter *w;
    while ((w = wq_pop(&rs))) {
        finish_recv(c, w->task, 0, NULL);
        slang_release(w->task);
        free(w);
    }
}
