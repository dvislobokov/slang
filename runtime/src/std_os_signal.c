/* std_os_signal.c - std.os.signal: signals delivered to tasks.
 *
 * The platform layer catches the signal (POSIX: a sigaction handler writes
 * to a self-pipe read by a delivery thread; Windows: SetConsoleCtrlHandler)
 * and calls deliver() on an ordinary thread, which completes the waiter
 * tasks registered for that signal (slang_task_rt_fire is thread-safe), or
 * counts the signal as pending when nobody waits, so a signal arriving
 * between two Waits of a Notify loop is not lost. Nothing here touches the
 * scheduler's poller (task_io.c / platform). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdlib.h>

slang_task *slang_task_rt_waker(void);
void slang_task_rt_fire(slang_task *t);

slang_task *slang_os_signal_rt_wait(int64_t sig);
void slang_os_signal_rt_ignore(int64_t sig);
void slang_os_signal_rt_reset(int64_t sig);
void slang_os_signal_rt_raise(int64_t sig);
int64_t slang_os_signal_rt_pending(int64_t sig);

#define NSIG_MAX 65

typedef struct waiter {
    slang_task *t;
    struct waiter *next;
} waiter;

static slang__plat_mutex mu = SLANG_PLAT_MUTEX_INIT;
static waiter *waiters[NSIG_MAX];
static int64_t pending[NSIG_MAX];
static bool armed[NSIG_MAX];

static void deliver(int sig) {
    if (sig <= 0 || sig >= NSIG_MAX) return;
    slang__plat_mutex_lock(&mu);
    waiter *w = waiters[sig];
    waiters[sig] = NULL;
    if (!w) pending[sig]++;
    slang__plat_mutex_unlock(&mu);
    while (w) {
        waiter *next = w->next;
        slang_task_rt_fire(w->t);
        slang_release(w->t);
        free(w);
        w = next;
    }
}

static void check(int64_t sig) {
    if (sig >= NSIG_MAX || !slang__plat_sig_catchable(sig)) slang_panic("signal: invalid or uncatchable signal");
}

/* Called with mu held. */
static void arm(int sig) {
    if (armed[sig]) return;
    slang__plat_sig_catch(sig);
    armed[sig] = true;
}

/* A task completed at the next delivery of sig (immediately if one is
 * pending). */
slang_task *slang_os_signal_rt_wait(int64_t sig) {
    check(sig);
    slang__plat_sig_start(deliver);
    slang_task *t = slang_task_rt_waker();
    slang__plat_mutex_lock(&mu);
    arm((int)sig);
    if (pending[sig] > 0) {
        pending[sig]--;
        slang__plat_mutex_unlock(&mu);
        slang_task_rt_fire(t);
        return t;
    }
    waiter *w = malloc(sizeof *w);
    if (!w) slang_panic("out of memory (signal)");
    slang_retain(t);
    w->t = t;
    w->next = waiters[sig];
    waiters[sig] = w;
    slang__plat_mutex_unlock(&mu);
    return t;
}

int64_t slang_os_signal_rt_pending(int64_t sig) {
    check(sig);
    slang__plat_mutex_lock(&mu);
    int64_t n = pending[sig];
    slang__plat_mutex_unlock(&mu);
    return n;
}

void slang_os_signal_rt_ignore(int64_t sig) {
    check(sig);
    slang__plat_mutex_lock(&mu);
    slang__plat_sig_ignore((int)sig);
    armed[sig] = false;
    pending[sig] = 0;
    slang__plat_mutex_unlock(&mu);
}

void slang_os_signal_rt_reset(int64_t sig) {
    check(sig);
    slang__plat_mutex_lock(&mu);
    slang__plat_sig_default((int)sig);
    armed[sig] = false;
    pending[sig] = 0;
    slang__plat_mutex_unlock(&mu);
}

void slang_os_signal_rt_raise(int64_t sig) {
    slang__plat_sig_raise((int)sig);
}

/* Replaces the default action of sig with queuing (no waiter yet). */
void slang_os_signal_rt_catch(int64_t sig);
void slang_os_signal_rt_catch(int64_t sig) {
    check(sig);
    slang__plat_sig_start(deliver);
    slang__plat_mutex_lock(&mu);
    arm((int)sig);
    slang__plat_mutex_unlock(&mu);
}
