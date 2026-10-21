/* poll_wsapoll.c - fallback readiness poller on Windows (platform.h):
 * WSAPoll over the armed sockets of the poller plus a loopback UDP socket
 * as the wake-up channel.
 *
 * The default backend is poll_iocp.c (IOCP + IOCTL_AFD_POLL); it falls
 * back to this one when \Device\Afd cannot be opened, or on request with
 * SLANG_POLLER=wsapoll (for comparisons). Both keep the same contract.
 * WSAPoll costs O(armed sockets) per wait and one wake-up per arm while a
 * wait is in progress; fine for clients and moderate servers.
 *
 * One-shot level triggering: arm() records the interest and token; wait()
 * snapshots the armed sockets, polls them, and reports (and disarms) each
 * socket that is ready and still armed with the same token. arm/remove
 * while a wait is running wake it so it re-snapshots. Only sockets can be
 * polled: arm() of anything else returns SLANG_PLAT_ENOTPOLLABLE (files are
 * always ready; process pipes are relayed to sockets by proc_win32.c). */
#include "platform.h"

#if defined(_WIN32)

#include "win32_util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct entry {
    SOCKET s;
    uint64_t token;
    int32_t interest;  /* 0 = disarmed */
} entry;

struct slang__w32_wsapoll {
    slang__plat_mutex mu;
    entry *e;
    int n, cap;
    SOCKET wake;          /* UDP socket connected to itself */
    volatile LONG waiting;  /* a wait is between snapshot and WSAPoll return */
    volatile LONG woken;    /* a wake datagram is in flight */
    volatile LONG epoch;    /* bumped whenever a WSAPoll call returns */
    WSAPOLLFD *pfd;       /* scratch of the waiting thread */
    uint64_t *ptok;
    int pcap;
};

slang__w32_wsapoll *slang__w32_wsapoll_new(void) {
    slang__w32_wsa_init();
    slang__w32_wsapoll *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    slang__plat_mutex_init(&p->mu);
    SOCKET s = WSASocketW(AF_INET, SOCK_DGRAM, 0, NULL, 0, 0x80 /* WSA_FLAG_NO_HANDLE_INHERIT */);
    if (s == INVALID_SOCKET) { free(p); return NULL; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof a;
    u_long one = 1;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(s, (struct sockaddr *)&a, &len) != 0 ||
        connect(s, (struct sockaddr *)&a, sizeof a) != 0 || ioctlsocket(s, FIONBIO, &one) != 0) {
        closesocket(s);
        free(p);
        return NULL;
    }
    p->wake = s;
    return p;
}

void slang__w32_wsapoll_free(slang__w32_wsapoll *p) {
    if (!p) return;
    closesocket(p->wake);
    free(p->e);
    free(p->pfd);
    free(p->ptok);
    free(p);
}

static void wake_if_waiting(slang__w32_wsapoll *p) {
    if (InterlockedCompareExchange(&p->waiting, 0, 0) && !InterlockedExchange(&p->woken, 1)) {
        char b = 1;
        send(p->wake, &b, 1, 0);
    }
}

static int find(slang__w32_wsapoll *p, SOCKET s) {
    for (int i = 0; i < p->n; i++)
        if (p->e[i].s == s) return i;
    return -1;
}

int slang__w32_wsapoll_arm(slang__w32_wsapoll *p, int64_t fd, int32_t interest, uint64_t token, bool registered) {
    (void)registered;
    SOCKET s = (SOCKET)fd;
    int type = 0, tl = sizeof type;
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &tl) != 0) {
        int e = WSAGetLastError();
        return e == WSAENOTSOCK ? SLANG_PLAT_ENOTPOLLABLE : EBADF;
    }
    slang__plat_mutex_lock(&p->mu);
    int i = find(p, s);
    if (i < 0) {
        if (p->n == p->cap) {
            int nc = p->cap ? p->cap * 2 : 16;
            entry *ne = realloc(p->e, sizeof *ne * (size_t)nc);
            if (!ne) { slang__plat_mutex_unlock(&p->mu); return ENOMEM; }
            p->e = ne;
            p->cap = nc;
        }
        i = p->n++;
        p->e[i].s = s;
    }
    p->e[i].interest = interest & (SLANG_PLAT_READ | SLANG_PLAT_WRITE);
    p->e[i].token = token;
    slang__plat_mutex_unlock(&p->mu);
    wake_if_waiting(p);
    return 0;
}

/* The caller closes fd next: a WSAPoll that is still waiting on the old
 * snapshot must not see the handle closed under it (Wine's WSAPoll can then
 * miss its timeout), so remove() wakes it and waits until it has returned. */
void slang__w32_wsapoll_remove(slang__w32_wsapoll *p, int64_t fd) {
    slang__plat_mutex_lock(&p->mu);
    int i = find(p, (SOCKET)fd);
    if (i >= 0) p->e[i] = p->e[--p->n];
    LONG e0 = InterlockedCompareExchange(&p->epoch, 0, 0);
    bool waiting = InterlockedCompareExchange(&p->waiting, 0, 0) != 0;
    slang__plat_mutex_unlock(&p->mu);
    if (!waiting) return;
    wake_if_waiting(p);
    for (int spins = 0; InterlockedCompareExchange(&p->epoch, 0, 0) == e0 && InterlockedCompareExchange(&p->waiting, 0, 0); spins++) {
        if (spins > 100000) break;  /* never block a close for good */
        if (spins > 64) Sleep(0);
        else YieldProcessor();
    }
}

int slang__w32_wsapoll_wait(slang__w32_wsapoll *p, slang__plat_event *evs, int max, int64_t timeout_ms) {
    slang__plat_mutex_lock(&p->mu);
    int need = p->n + 1;
    if (need > p->pcap) {
        int nc = need * 2;
        WSAPOLLFD *nf = realloc(p->pfd, sizeof *nf * (size_t)nc);
        if (nf) p->pfd = nf;
        uint64_t *nt = realloc(p->ptok, sizeof *nt * (size_t)nc);
        if (nt) p->ptok = nt;
        if (nf && nt) p->pcap = nc;
    }
    int k = 0;
    p->pfd[k].fd = p->wake;
    p->pfd[k].events = POLLRDNORM;
    p->pfd[k].revents = 0;
    p->ptok[k++] = SLANG_PLAT_WAKE_TOKEN;
    for (int i = 0; i < p->n && k < p->pcap; i++) {
        if (!p->e[i].interest) continue;
        p->pfd[k].fd = p->e[i].s;
        p->pfd[k].events = (SHORT)(((p->e[i].interest & SLANG_PLAT_READ) ? POLLRDNORM : 0) |
                                   ((p->e[i].interest & SLANG_PLAT_WRITE) ? POLLWRNORM : 0));
        p->pfd[k].revents = 0;
        p->ptok[k++] = p->e[i].token;
    }
    InterlockedExchange(&p->waiting, 1);
    slang__plat_mutex_unlock(&p->mu);
    int t = timeout_ms < 0 ? -1 : timeout_ms > 0x7fffffff ? 0x7fffffff : (int)timeout_ms;
    int r = WSAPoll(p->pfd, (ULONG)k, t);
    InterlockedExchange(&p->waiting, 0);
    InterlockedIncrement(&p->epoch);
    if (r <= 0) return 0;
    int out = 0;
    slang__plat_mutex_lock(&p->mu);
    for (int j = 0; j < k && out < max; j++) {
        SHORT re = p->pfd[j].revents;
        if (!re) continue;
        if (j == 0) {
            char b[64];
            while (recv(p->wake, b, sizeof b, 0) > 0) {}
            InterlockedExchange(&p->woken, 0);
            evs[out].token = SLANG_PLAT_WAKE_TOKEN;
            evs[out++].events = SLANG_PLAT_READ;
            continue;
        }
        int i = find(p, p->pfd[j].fd);
        if (i < 0 || !p->e[i].interest || p->e[i].token != p->ptok[j]) continue;  /* re-armed or removed meanwhile */
        int32_t ev = 0;
        if (re & (POLLRDNORM | POLLHUP)) ev |= SLANG_PLAT_READ;
        if (re & POLLWRNORM) ev |= SLANG_PLAT_WRITE;
        if (re & (POLLERR | POLLNVAL)) ev |= SLANG_PLAT_ERROR | SLANG_PLAT_READ | SLANG_PLAT_WRITE;
        if (re & POLLHUP) ev |= SLANG_PLAT_WRITE;
        p->e[i].interest = 0;  /* one-shot */
        evs[out].token = p->ptok[j];
        evs[out++].events = ev;
    }
    slang__plat_mutex_unlock(&p->mu);
    return out;
}

void slang__w32_wsapoll_wake(slang__w32_wsapoll *p) {
    if (!InterlockedExchange(&p->woken, 1)) {
        char b = 1;
        send(p->wake, &b, 1, 0);
    }
}

#else
typedef int slang__poll_wsapoll_unused;
#endif
