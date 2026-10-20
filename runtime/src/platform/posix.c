/* posix.c - POSIX implementation of the platform layer (Linux, macOS, BSD):
 * threads, CPU count, monotonic clock, entropy and backtraces. The
 * readiness poller is in poll_epoll.c / poll_kqueue.c. */
#include "platform.h"

#if defined(__unix__) || defined(__APPLE__)

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif

_Static_assert(sizeof(pthread_t) <= sizeof(uintptr_t), "pthread_t must fit in uintptr_t");

bool slang__plat_thread_start(slang__plat_thread *t, slang__plat_thread_fn fn, void *arg) {
    pthread_t th;
    if (pthread_create(&th, NULL, fn, arg) != 0) return false;
    t->handle = (uintptr_t)th;
    return true;
}

bool slang__plat_thread_start_detached(slang__plat_thread_fn fn, void *arg) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

void slang__plat_thread_join(slang__plat_thread t) {
    pthread_join((pthread_t)t.handle, NULL);
}

int64_t slang__plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int64_t)n : 1;
}

void slang__plat_yield(void) { sched_yield(); }

int64_t slang__plat_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

bool slang__plat_random_bytes(void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
#if defined(__linux__)
    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        got += (size_t)r;
    }
    if (got == n) return true;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    while (got < n) {
        size_t k = n - got > 256 ? 256 : n - got;
        if (getentropy(p + got, k) != 0) break;
        got += k;
    }
    if (got == n) return true;
#endif
    /* Fallback: /dev/urandom. */
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            close(fd);
            return false;
        }
        got += (size_t)r;
    }
    close(fd);
    return true;
}

void slang__plat_backtrace(int fd, int skip) {
    void *frames[64];
    int n = backtrace(frames, 64);
    if (n > skip) backtrace_symbols_fd(frames + skip, n - skip, fd);
}

#else
#error "platform/posix.c: unsupported platform (a win32.c implementation is planned)"
#endif
