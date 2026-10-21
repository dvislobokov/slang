/* poll_kqueue.c - readiness poller for macOS and the BSDs (platform.h).
 *
 * Every arm adds EVFILT_READ and/or EVFILT_WRITE with EV_ONESHOT and
 * without EV_CLEAR: level-triggered, one report per arm, exactly like the
 * epoll backend (EPOLLONESHOT). kqueue keeps the two directions as two
 * filters, so after a READ report a WRITE filter armed by the same call is
 * still pending; its later report is a harmless spurious one for the
 * caller (task_io.c ignores directions nobody waits for). Wake-ups use
 * EVFILT_USER where it exists (macOS, FreeBSD, NetBSD >= 10, DragonFly)
 * and a non-blocking self-pipe elsewhere (OpenBSD). Tokens travel in
 * udata, which is pointer-sized (64-bit hosts carry the whole token). */
#include "platform.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(EVFILT_USER)
#define SLANG_KQ_USER 1
#else
#define SLANG_KQ_USER 0
#endif

struct slang__plat_poller {
    int kq;
    int pipe_rd, pipe_wr;  /* self-pipe when there is no EVFILT_USER */
};

#define WAKE_IDENT 0x5157  /* EVFILT_USER ident of the wake-up event */

static void set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

slang__plat_poller *slang__plat_poller_new(void) {
    slang__plat_poller *p = malloc(sizeof *p);
    if (!p) return NULL;
    p->kq = kqueue();
    p->pipe_rd = p->pipe_wr = -1;
    if (p->kq < 0) {
        free(p);
        return NULL;
    }
    fcntl(p->kq, F_SETFD, FD_CLOEXEC);
    struct kevent kev;
#if SLANG_KQ_USER
    EV_SET(&kev, WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, (void *)(uintptr_t)SLANG_PLAT_WAKE_TOKEN);
    if (kevent(p->kq, &kev, 1, NULL, 0, NULL) != 0) {
        close(p->kq);
        free(p);
        return NULL;
    }
#else
    int fds[2];
    if (pipe(fds) != 0) {
        close(p->kq);
        free(p);
        return NULL;
    }
    set_nonblock_cloexec(fds[0]);
    set_nonblock_cloexec(fds[1]);
    p->pipe_rd = fds[0];
    p->pipe_wr = fds[1];
    EV_SET(&kev, (uintptr_t)p->pipe_rd, EVFILT_READ, EV_ADD, 0, 0, (void *)(uintptr_t)SLANG_PLAT_WAKE_TOKEN);
    if (kevent(p->kq, &kev, 1, NULL, 0, NULL) != 0) {
        close(p->kq);
        close(fds[0]);
        close(fds[1]);
        free(p);
        return NULL;
    }
#endif
    (void)set_nonblock_cloexec;
    return p;
}

void slang__plat_poller_free(slang__plat_poller *p) {
    if (!p) return;
    close(p->kq);
    if (p->pipe_rd >= 0) close(p->pipe_rd);
    if (p->pipe_wr >= 0) close(p->pipe_wr);
    free(p);
}

int slang__plat_poller_arm(slang__plat_poller *p, int64_t fd, int32_t interest, uint64_t token, bool registered) {
    (void)registered;
    struct kevent ch[2], res[2];
    int n = 0;
    void *ud = (void *)(uintptr_t)token;
    if (interest == 0) {
        slang__plat_poller_remove(p, fd);
        return 0;
    }
    if (interest & SLANG_PLAT_READ) {
        EV_SET(&ch[n], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT | EV_RECEIPT, 0, 0, ud);
        n++;
    }
    if (interest & SLANG_PLAT_WRITE) {
        EV_SET(&ch[n], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_ONESHOT | EV_RECEIPT, 0, 0, ud);
        n++;
    }
    int r;
    do r = kevent(p->kq, ch, n, res, n, NULL);
    while (r < 0 && errno == EINTR);
    if (r < 0) return errno;
    for (int i = 0; i < r; i++)
        if ((res[i].flags & EV_ERROR) && res[i].data != 0) return (int)res[i].data;
    return 0;
}

void slang__plat_poller_remove(slang__plat_poller *p, int64_t fd) {
    struct kevent ch[2], res[2];
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    int r;
    do r = kevent(p->kq, ch, 2, res, 2, NULL);  /* ENOENT for filters not added: fine */
    while (r < 0 && errno == EINTR);
}

int slang__plat_poller_wait(slang__plat_poller *p, slang__plat_event *evs, int max, int64_t timeout_ms) {
    struct kevent buf[256];
    if (max > 256) max = 256;
    if (max < 1) return 0;
    struct timespec ts, *tp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = (time_t)(timeout_ms / 1000);
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tp = &ts;
    }
    int n = kevent(p->kq, NULL, 0, buf, max, tp);
    if (n < 0) return 0;  /* EINTR */
    int out = 0;
    for (int i = 0; i < n; i++) {
        uint64_t token = (uint64_t)(uintptr_t)buf[i].udata;
        if (token == (uint64_t)(uintptr_t)SLANG_PLAT_WAKE_TOKEN) {
            if (p->pipe_rd >= 0) {
                char drain[64];
                while (read(p->pipe_rd, drain, sizeof drain) > 0) {}
            }
            evs[out].token = SLANG_PLAT_WAKE_TOKEN;
            evs[out].events = 0;
            out++;
            continue;
        }
        if ((buf[i].flags & EV_ERROR) && buf[i].data != 0) {
            evs[out].token = token;
            evs[out].events = SLANG_PLAT_ERROR | SLANG_PLAT_READ | SLANG_PLAT_WRITE;
            out++;
            continue;
        }
        int32_t e = buf[i].filter == EVFILT_READ ? SLANG_PLAT_READ : buf[i].filter == EVFILT_WRITE ? SLANG_PLAT_WRITE : 0;
        if (buf[i].flags & EV_EOF) e |= buf[i].fflags ? SLANG_PLAT_ERROR | SLANG_PLAT_READ | SLANG_PLAT_WRITE : 0;
        evs[out].token = token;
        evs[out].events = e;
        out++;
    }
    return out;
}

void slang__plat_poller_wake(slang__plat_poller *p) {
#if SLANG_KQ_USER
    struct kevent kev;
    EV_SET(&kev, WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, (void *)(uintptr_t)SLANG_PLAT_WAKE_TOKEN);
    int r;
    do r = kevent(p->kq, &kev, 1, NULL, 0, NULL);
    while (r < 0 && errno == EINTR);
#else
    char one = 1;
    ssize_t r;
    do r = write(p->pipe_wr, &one, 1);
    while (r < 0 && errno == EINTR);
#endif
}

const char *slang__plat_poller_backend(slang__plat_poller *p) {
    (void)p;
    return "kqueue";
}

#else
/* Not macOS/BSD: the poller comes from poll_epoll.c. */
typedef int slang__poll_kqueue_unused;
#endif
