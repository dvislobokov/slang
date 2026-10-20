/* task_io.c - readiness waits for async I/O (ABI.md 10.9).
 *
 * One I/O thread runs the platform poller (epoll on Linux, see
 * platform/poll_epoll.c). slang_io_wait_readable/writable(fd, timeout)
 * register a pending Task<int> for the descriptor; when the poller reports
 * readiness (or the deadline passes, or slang_io_forget releases the fd)
 * the I/O thread completes the task, which posts its continuations to the
 * scheduler exactly like a timer does. At most one reader and one writer
 * may wait on a descriptor at a time. The descriptor itself must be in
 * non-blocking mode; the poller is level-triggered and an fd is only
 * registered while somebody waits on it. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum { IO_READY = 0, IO_TIMEOUT = 1, IO_CLOSED = 2, IO_CANCELLED = 3 };

typedef struct io_wait {
    slang_task *task;     /* owned, NULL = nobody waits */
    int64_t deadline;     /* monotonic ns, 0 = none */
} io_wait;

typedef struct io_fd {
    io_wait rd, wr;
    bool registered;
} io_fd;

typedef struct io_done {
    slang_task *task;     /* owned */
    int64_t code;
} io_done;

static pthread_mutex_t io_mu = PTHREAD_MUTEX_INITIALIZER;
static io_fd *io_tab;
static int64_t io_cap;
static slang__plat_poller *io_poller;
static int io_running, io_stopping;
static slang__plat_thread io_thread;

/* io_mu held. */
static void interest_update(int64_t fd) {
    io_fd *e = &io_tab[fd];
    int32_t want = (e->rd.task ? SLANG_PLAT_READ : 0) | (e->wr.task ? SLANG_PLAT_WRITE : 0);
    if (want == 0) {
        if (e->registered) slang__plat_poller_remove(io_poller, fd);
        e->registered = false;
        return;
    }
    if (slang__plat_poller_set(io_poller, fd, want, e->registered) == 0) e->registered = true;
}

/* io_mu held: moves the waiter in w (if any) to the done list. */
static void take(io_wait *w, int64_t code, io_done *out, int *n) {
    if (!w->task) return;
    out[*n].task = w->task;
    out[*n].code = code;
    (*n)++;
    w->task = NULL;
    w->deadline = 0;
}

static void finish(io_done *d, int n) {
    for (int i = 0; i < n; i++) {
        slang_task_complete(d[i].task, &d[i].code);
        slang_release(d[i].task);
    }
}

static void *io_main(void *unused) {
    (void)unused;
    slang__plat_event evs[64];
    io_done *done = NULL;
    int done_cap = 0;
    for (;;) {
        pthread_mutex_lock(&io_mu);
        if (io_stopping) { pthread_mutex_unlock(&io_mu); break; }
        int64_t next = 0;
        for (int64_t fd = 0; fd < io_cap; fd++) {
            int64_t a = io_tab[fd].rd.task ? io_tab[fd].rd.deadline : 0;
            int64_t b = io_tab[fd].wr.task ? io_tab[fd].wr.deadline : 0;
            if (a && (!next || a < next)) next = a;
            if (b && (!next || b < next)) next = b;
        }
        pthread_mutex_unlock(&io_mu);
        int64_t timeout = -1;
        if (next) {
            int64_t left = next - slang__plat_mono_ns();
            timeout = left <= 0 ? 0 : (left + 999999) / 1000000;
        }
        int n = slang__plat_poller_wait(io_poller, evs, 64, timeout);

        pthread_mutex_lock(&io_mu);
        int need = 2 * n + 2 * (int)io_cap;
        if (need > done_cap) {
            done_cap = need + 16;
            done = realloc(done, sizeof *done * (size_t)done_cap);
            if (!done) slang_panic("out of memory (io)");
        }
        int nd = 0;
        for (int i = 0; i < n; i++) {
            int64_t fd = evs[i].fd;
            if (fd < 0 || fd >= io_cap) continue;
            io_fd *e = &io_tab[fd];
            if (evs[i].events & SLANG_PLAT_READ) take(&e->rd, IO_READY, done, &nd);
            if (evs[i].events & SLANG_PLAT_WRITE) take(&e->wr, IO_READY, done, &nd);
            interest_update(fd);
        }
        int64_t now = slang__plat_mono_ns();
        for (int64_t fd = 0; fd < io_cap; fd++) {
            io_fd *e = &io_tab[fd];
            bool changed = false;
            if (e->rd.task && e->rd.deadline && e->rd.deadline <= now) { take(&e->rd, IO_TIMEOUT, done, &nd); changed = true; }
            if (e->wr.task && e->wr.deadline && e->wr.deadline <= now) { take(&e->wr, IO_TIMEOUT, done, &nd); changed = true; }
            if (changed) interest_update(fd);
        }
        pthread_mutex_unlock(&io_mu);
        finish(done, nd);
    }
    free(done);
    return NULL;
}

/* io_mu held. */
static void io_start_locked(void) {
    if (io_running) return;
    io_poller = slang__plat_poller_new();
    if (!io_poller) slang_panic("cannot create the I/O poller");
    io_stopping = 0;
    if (!slang__plat_thread_start(&io_thread, io_main, NULL)) slang_panic("cannot create the I/O thread");
    io_running = 1;
}

/* Cancellation hook: completes the wait w with IO_CANCELLED. */
static void io_cancelled(slang_task *w) {
    io_done d[1];
    int nd = 0;
    pthread_mutex_lock(&io_mu);
    for (int64_t fd = 0; fd < io_cap && nd == 0; fd++) {
        io_fd *e = &io_tab[fd];
        if (e->rd.task == w) take(&e->rd, IO_CANCELLED, d, &nd);
        else if (e->wr.task == w) take(&e->wr, IO_CANCELLED, d, &nd);
        if (nd) interest_update(fd);
    }
    pthread_mutex_unlock(&io_mu);
    finish(d, nd);
}

static slang_task *io_wait_for(int64_t fd, bool write, int64_t timeout_ms) {
    if (fd < 0) slang_panic("slang_io_wait: invalid file descriptor");
    slang_task *t = slang_task_new_plain(8, NULL);
    pthread_mutex_lock(&io_mu);
    io_start_locked();
    if (fd >= io_cap) {
        int64_t nc = io_cap ? io_cap : 64;
        while (nc <= fd) nc *= 2;
        io_fd *nt = realloc(io_tab, sizeof *nt * (size_t)nc);
        if (!nt) slang_panic("out of memory (io)");
        memset(nt + io_cap, 0, sizeof *nt * (size_t)(nc - io_cap));
        io_tab = nt;
        io_cap = nc;
    }
    io_fd *e = &io_tab[fd];
    io_wait *w = write ? &e->wr : &e->rd;
    if (w->task) {
        pthread_mutex_unlock(&io_mu);
        slang__panicf("two tasks wait to %s file descriptor %lld at the same time",
                      write ? "write" : "read", (long long)fd);
    }
    slang_retain(t);
    w->task = t;
    w->deadline = timeout_ms >= 0 ? slang__plat_mono_ns() + timeout_ms * 1000000 + 1 : 0;
    int32_t want = (e->rd.task ? SLANG_PLAT_READ : 0) | (e->wr.task ? SLANG_PLAT_WRITE : 0);
    int rc = slang__plat_poller_set(io_poller, fd, want, e->registered);
    io_done d[1];
    int nd = 0;
    if (rc == 0) {
        e->registered = true;
    } else {
        /* Not pollable (a regular file, say): always ready. */
        take(w, IO_READY, d, &nd);
    }
    bool wake = w->deadline != 0;
    pthread_mutex_unlock(&io_mu);
    if (wake) slang__plat_poller_wake(io_poller);
    finish(d, nd);
    if (!nd) slang__cancel_hook(io_cancelled, t);
    return t;
}

slang_task *slang_io_wait_readable(int64_t fd, int64_t timeout_ms) { return io_wait_for(fd, false, timeout_ms); }
slang_task *slang_io_wait_writable(int64_t fd, int64_t timeout_ms) { return io_wait_for(fd, true, timeout_ms); }

void slang_io_forget(int64_t fd) {
    io_done d[2];
    int nd = 0;
    pthread_mutex_lock(&io_mu);
    if (fd >= 0 && fd < io_cap) {
        io_fd *e = &io_tab[fd];
        take(&e->rd, IO_CLOSED, d, &nd);
        take(&e->wr, IO_CLOSED, d, &nd);
        if (e->registered) slang__plat_poller_remove(io_poller, fd);
        e->registered = false;
    }
    pthread_mutex_unlock(&io_mu);
    finish(d, nd);
}

void slang__io_stop(void) {
    pthread_mutex_lock(&io_mu);
    if (!io_running) { pthread_mutex_unlock(&io_mu); return; }
    io_stopping = 1;
    pthread_mutex_unlock(&io_mu);
    slang__plat_poller_wake(io_poller);
    slang__plat_thread_join(io_thread);
    pthread_mutex_lock(&io_mu);
    /* Waits still pending at shutdown are dropped without completing. */
    for (int64_t fd = 0; fd < io_cap; fd++) {
        if (io_tab[fd].rd.task) slang_release(io_tab[fd].rd.task);
        if (io_tab[fd].wr.task) slang_release(io_tab[fd].wr.task);
    }
    free(io_tab);
    io_tab = NULL;
    io_cap = 0;
    slang__plat_poller_free(io_poller);
    io_poller = NULL;
    io_running = 0;
    pthread_mutex_unlock(&io_mu);
}
