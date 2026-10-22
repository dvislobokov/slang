/* std_os_signal.c - std.os.signal: signals delivered to tasks.
 *
 * The platform layer catches the signal (POSIX: a sigaction handler writes
 * to a self-pipe read by a delivery thread; Windows: SetConsoleCtrlHandler)
 * and calls deliver() on an ordinary thread, which completes the waiter
 * tasks registered for that signal (zell_task_rt_fire is thread-safe), or
 * counts the signal as pending when nobody waits, so a signal arriving
 * between two Waits of a Notify loop is not lost. Nothing here touches the
 * scheduler's poller (task_io.c / platform). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdlib.h>

zell_task *zell_task_rt_waker(void);
void zell_task_rt_fire(zell_task *t);

zell_task *zell_os_signal_rt_wait(int64_t sig);
void zell_os_signal_rt_ignore(int64_t sig);
void zell_os_signal_rt_reset(int64_t sig);
void zell_os_signal_rt_raise(int64_t sig);
int64_t zell_os_signal_rt_pending(int64_t sig);

#define NSIG_MAX 65

typedef struct waiter {
    zell_task *t;
    struct waiter *next;
} waiter;

static zell__plat_mutex mu = ZELL_PLAT_MUTEX_INIT;
static waiter *waiters[NSIG_MAX];
static int64_t pending[NSIG_MAX];
static bool armed[NSIG_MAX];

static void deliver(int sig) {
    if (sig <= 0 || sig >= NSIG_MAX) return;
    zell__plat_mutex_lock(&mu);
    waiter *w = waiters[sig];
    waiters[sig] = NULL;
    if (!w) pending[sig]++;
    zell__plat_mutex_unlock(&mu);
    while (w) {
        waiter *next = w->next;
        zell_task_rt_fire(w->t);
        zell_release(w->t);
        free(w);
        w = next;
    }
}

static void check(int64_t sig) {
    if (sig >= NSIG_MAX || !zell__plat_sig_catchable(sig)) zell_panic("signal: invalid or uncatchable signal");
}

/* Called with mu held. */
static void arm(int sig) {
    if (armed[sig]) return;
    zell__plat_sig_catch(sig);
    armed[sig] = true;
}

/* A task completed at the next delivery of sig (immediately if one is
 * pending). */
zell_task *zell_os_signal_rt_wait(int64_t sig) {
    check(sig);
    zell__plat_sig_start(deliver);
    zell_task *t = zell_task_rt_waker();
    zell__plat_mutex_lock(&mu);
    arm((int)sig);
    if (pending[sig] > 0) {
        pending[sig]--;
        zell__plat_mutex_unlock(&mu);
        zell_task_rt_fire(t);
        return t;
    }
    waiter *w = malloc(sizeof *w);
    if (!w) zell_panic("out of memory (signal)");
    zell_retain(t);
    w->t = t;
    w->next = waiters[sig];
    waiters[sig] = w;
    zell__plat_mutex_unlock(&mu);
    return t;
}

int64_t zell_os_signal_rt_pending(int64_t sig) {
    check(sig);
    zell__plat_mutex_lock(&mu);
    int64_t n = pending[sig];
    zell__plat_mutex_unlock(&mu);
    return n;
}

void zell_os_signal_rt_ignore(int64_t sig) {
    check(sig);
    zell__plat_mutex_lock(&mu);
    zell__plat_sig_ignore((int)sig);
    armed[sig] = false;
    pending[sig] = 0;
    zell__plat_mutex_unlock(&mu);
}

void zell_os_signal_rt_reset(int64_t sig) {
    check(sig);
    zell__plat_mutex_lock(&mu);
    zell__plat_sig_default((int)sig);
    armed[sig] = false;
    pending[sig] = 0;
    zell__plat_mutex_unlock(&mu);
}

void zell_os_signal_rt_raise(int64_t sig) {
    zell__plat_sig_raise((int)sig);
}

/* Replaces the default action of sig with queuing (no waiter yet). */
void zell_os_signal_rt_catch(int64_t sig);
void zell_os_signal_rt_catch(int64_t sig) {
    check(sig);
    zell__plat_sig_start(deliver);
    zell__plat_mutex_lock(&mu);
    arm((int)sig);
    zell__plat_mutex_unlock(&mu);
}
