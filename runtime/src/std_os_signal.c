/* std_os_signal.c - std.os.signal: POSIX signals delivered to tasks.
 *
 * A handler installed with sigaction writes the signal number to a
 * self-pipe (async-signal-safe); a dedicated thread reads the pipe and
 * completes the waiter tasks registered for that signal (slang_task_rt_fire
 * is thread-safe), or counts the signal as pending when nobody waits, so a
 * signal arriving between two Waits of a Notify loop is not lost. Nothing
 * here touches the scheduler's poller (task_io.c / platform). */
#include "rt_internal.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int pipefd[2] = {-1, -1};
static waiter *waiters[NSIG_MAX];
static int64_t pending[NSIG_MAX];
static bool armed[NSIG_MAX];

static void on_signal(int sig) {
    int saved = errno;
    unsigned char b = (unsigned char)sig;
    ssize_t r = write(pipefd[1], &b, 1);
    (void)r;
    errno = saved;
}

static void *reader(void *arg) {
    (void)arg;
    /* this thread never takes signals itself */
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    for (;;) {
        unsigned char b;
        ssize_t n = read(pipefd[0], &b, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return NULL;
        int sig = b;
        if (sig <= 0 || sig >= NSIG_MAX) continue;
        pthread_mutex_lock(&mu);
        waiter *w = waiters[sig];
        waiters[sig] = NULL;
        if (!w) pending[sig]++;
        pthread_mutex_unlock(&mu);
        while (w) {
            waiter *next = w->next;
            slang_task_rt_fire(w->t);
            slang_release(w->t);
            free(w);
            w = next;
        }
    }
}

static void init(void) {
    if (pipe(pipefd) != 0) slang_panic("signal: cannot create pipe");
    pthread_t th;
    if (pthread_create(&th, NULL, reader, NULL) != 0) slang_panic("signal: cannot start thread");
    pthread_detach(th);
}

static void check(int64_t sig) {
    if (sig <= 0 || sig >= NSIG_MAX || sig == SIGKILL || sig == SIGSTOP) slang_panic("signal: invalid or uncatchable signal");
}

/* Called with mu held. */
static void arm(int sig) {
    if (armed[sig]) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, NULL);
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, sig);
    pthread_sigmask(SIG_UNBLOCK, &one, NULL);
    armed[sig] = true;
}

/* A task completed at the next delivery of sig (immediately if one is
 * pending). */
slang_task *slang_os_signal_rt_wait(int64_t sig) {
    check(sig);
    pthread_once(&once, init);
    slang_task *t = slang_task_rt_waker();
    pthread_mutex_lock(&mu);
    arm((int)sig);
    if (pending[sig] > 0) {
        pending[sig]--;
        pthread_mutex_unlock(&mu);
        slang_task_rt_fire(t);
        return t;
    }
    waiter *w = malloc(sizeof *w);
    if (!w) slang_panic("out of memory (signal)");
    slang_retain(t);
    w->t = t;
    w->next = waiters[sig];
    waiters[sig] = w;
    pthread_mutex_unlock(&mu);
    return t;
}

int64_t slang_os_signal_rt_pending(int64_t sig) {
    check(sig);
    pthread_mutex_lock(&mu);
    int64_t n = pending[sig];
    pthread_mutex_unlock(&mu);
    return n;
}

void slang_os_signal_rt_ignore(int64_t sig) {
    check(sig);
    pthread_mutex_lock(&mu);
    signal((int)sig, SIG_IGN);
    armed[sig] = false;
    pending[sig] = 0;
    pthread_mutex_unlock(&mu);
}

void slang_os_signal_rt_reset(int64_t sig) {
    check(sig);
    pthread_mutex_lock(&mu);
    signal((int)sig, SIG_DFL);
    armed[sig] = false;
    pending[sig] = 0;
    pthread_mutex_unlock(&mu);
}

void slang_os_signal_rt_raise(int64_t sig) {
    kill(getpid(), (int)sig);
}

/* Replaces the default action of sig with queuing (no waiter yet). */
void slang_os_signal_rt_catch(int64_t sig);
void slang_os_signal_rt_catch(int64_t sig) {
    check(sig);
    pthread_once(&once, init);
    pthread_mutex_lock(&mu);
    arm((int)sig);
    pthread_mutex_unlock(&mu);
}
