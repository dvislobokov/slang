/* poll_epoll.c - readiness poller on Linux (platform.h): level-triggered,
 * one-shot epoll (EPOLLONESHOT re-armed with EPOLL_CTL_MOD for every wait)
 * plus an eventfd for cross-thread wake-ups. */
#include "platform.h"

#if defined(__linux__)

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct slang__plat_poller {
    int epfd;
    int wakefd;
};

slang__plat_poller *slang__plat_poller_new(void) {
    slang__plat_poller *p = malloc(sizeof *p);
    if (!p) return NULL;
    p->epfd = epoll_create1(EPOLL_CLOEXEC);
    p->wakefd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (p->epfd < 0 || p->wakefd < 0) {
        if (p->epfd >= 0) close(p->epfd);
        if (p->wakefd >= 0) close(p->wakefd);
        free(p);
        return NULL;
    }
    /* The wake-up channel stays armed (level-triggered, not one-shot). */
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = SLANG_PLAT_WAKE_TOKEN };
    if (epoll_ctl(p->epfd, EPOLL_CTL_ADD, p->wakefd, &ev) != 0) {
        close(p->epfd);
        close(p->wakefd);
        free(p);
        return NULL;
    }
    return p;
}

void slang__plat_poller_free(slang__plat_poller *p) {
    if (!p) return;
    close(p->epfd);
    close(p->wakefd);
    free(p);
}

int slang__plat_poller_arm(slang__plat_poller *p, int64_t fd, int32_t interest, uint64_t token, bool registered) {
    struct epoll_event ev = { .events = 0, .data.u64 = token };
    if (interest & SLANG_PLAT_READ) ev.events |= EPOLLIN | EPOLLRDHUP;
    if (interest & SLANG_PLAT_WRITE) ev.events |= EPOLLOUT;
    if (ev.events) ev.events |= EPOLLONESHOT;
    int op = registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    int rc = epoll_ctl(p->epfd, op, (int)fd, &ev);
    if (rc != 0 && errno == ENOENT && registered) rc = epoll_ctl(p->epfd, EPOLL_CTL_ADD, (int)fd, &ev);
    else if (rc != 0 && errno == EEXIST && !registered) rc = epoll_ctl(p->epfd, EPOLL_CTL_MOD, (int)fd, &ev);
    if (rc == 0) return 0;
    return errno == EPERM ? SLANG_PLAT_ENOTPOLLABLE : errno;
}

void slang__plat_poller_remove(slang__plat_poller *p, int64_t fd) {
    struct epoll_event ev = { 0 };
    epoll_ctl(p->epfd, EPOLL_CTL_DEL, (int)fd, &ev);
}

int slang__plat_poller_wait(slang__plat_poller *p, slang__plat_event *evs, int max, int64_t timeout_ms) {
    struct epoll_event buf[256];
    if (max > 256) max = 256;
    if (max < 1) return 0;
    int tmo = timeout_ms < 0 ? -1 : timeout_ms > 0x7fffffff ? 0x7fffffff : (int)timeout_ms;
    int n = epoll_wait(p->epfd, buf, max, tmo);
    if (n < 0) return 0;  /* EINTR (a signal): the caller loops */
    int out = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i].data.u64 == SLANG_PLAT_WAKE_TOKEN) {
            uint64_t v;
            while (read(p->wakefd, &v, sizeof v) > 0) {}
            evs[out].token = SLANG_PLAT_WAKE_TOKEN;
            evs[out].events = 0;
            out++;
            continue;
        }
        int32_t e = 0;
        if (buf[i].events & (EPOLLIN | EPOLLRDHUP)) e |= SLANG_PLAT_READ;
        if (buf[i].events & EPOLLOUT) e |= SLANG_PLAT_WRITE;
        if (buf[i].events & (EPOLLERR | EPOLLHUP)) e |= SLANG_PLAT_ERROR | SLANG_PLAT_READ | SLANG_PLAT_WRITE;
        evs[out].token = buf[i].data.u64;
        evs[out].events = e;
        out++;
    }
    return out;
}

void slang__plat_poller_wake(slang__plat_poller *p) {
    uint64_t one = 1;
    for (;;) {
        ssize_t r = write(p->wakefd, &one, sizeof one);
        if (r >= 0 || errno != EINTR) break;  /* EAGAIN: counter saturated, already pending */
    }
}

#else
/* Not Linux: the poller comes from poll_kqueue.c. */
typedef int slang__poll_epoll_unused;
#endif
