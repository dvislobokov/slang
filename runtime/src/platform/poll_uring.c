/* poll_uring.c - io_uring readiness backend for the Linux poller
 * (platform.h "readiness poller"; poll_epoll.c delegates to it when
 * slang__uring_new succeeds).
 *
 * Linux 6.0+ (IORING_FEAT_EXT_ARG for a timed wait, IORING_FEAT_NODROP,
 * IORING_REGISTER_SYNC_CANCEL so that removing a descriptor before close
 * drops the ring's reference to it at once).
 * liburing is not used: the few kernel structures needed are declared here
 * and the three system calls are made directly. The semantics are those of
 * the epoll backend: one-shot, level-triggered readiness. poller_arm
 * submits an IORING_OP_POLL_ADD (one report, then disarmed), replacing a
 * pending one with IORING_OP_POLL_REMOVE; poller_wait waits for
 * completions with a timeout; an eventfd polled the same way is the wake-up
 * channel. Every poll carries user_data = fd << 32 | seq, and a per-fd
 * sequence number tells a completion of the current arm from that of a
 * replaced or removed one (dropped: reports may be spurious anyway, never
 * lost, because every arm is level-triggered).
 *
 * Selection: opt-in with SLANG_IO_BACKEND=io_uring, and only when the
 * kernel supports it (io_uring_setup can be refused by seccomp or sysctl
 * kernel.io_uring_disabled; then epoll is used).
 *
 * Close timing: a pending poll is cancelled synchronously before close
 * (slang__uring_remove), but a poll that has just completed may still hold
 * its file reference for a moment (the submitting task frees completed
 * requests right after posting their CQEs, so the waiter can see the CQE
 * and close first). Then close(2) does not release the socket yet, and a
 * listener closed that way could still accept and reset a connection (2%
 * of examples/tcp_echo runs under gVisor). std.net therefore shuts a
 * listener down (shutdown(SHUT_RD), which stops listening whatever holds
 * the file) before closing it; for connected sockets the delay only
 * postpones the FIN by microseconds. examples/tcp_echo: 0 of 500 runs
 * differ (Linux 6.18).
 *
 * Not the default (benchmarks/README.md, 2026-10-21): on tcp_echo it is at
 * parity with one-shot epoll (best 0.431 vs 0.456 s, median 0.536 vs
 * 0.558 s over 12 runs each), but http_hello serves 20% fewer requests
 * (67-73k vs 87-91k req/s with Go's load generator): every wait costs one
 * POLL_ADD submission (an io_uring_enter, like one epoll_ctl) plus a
 * completion reaped under the ring lock, and keep-alive connections
 * re-arm after every request. Submitting outside the lock made no
 * difference. Multishot polls / receives would be the next step. */
#include "platform.h"

#if defined(__linux__)

#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif
#ifndef POLLRDHUP
#define POLLRDHUP 0x2000
#endif

/* ---- the kernel ABI (include/uapi/linux/io_uring.h), minimal ---- */

struct slu_sq_off { uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, resv1; uint64_t user_addr; };
struct slu_cq_off { uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1; uint64_t user_addr; };
struct slu_params {
    uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle, features, wq_fd, resv[3];
    struct slu_sq_off sq_off;
    struct slu_cq_off cq_off;
};
struct slu_sqe {                     /* 64 bytes */
    uint8_t opcode, flags;
    uint16_t ioprio;
    int32_t fd;
    uint64_t off;
    uint64_t addr;
    uint32_t len;
    uint32_t op_flags;               /* poll32_events for POLL_ADD */
    uint64_t user_data;
    uint16_t buf_index, personality;
    int32_t splice_fd_in;
    uint64_t addr3, pad2;
};
struct slu_cqe { uint64_t user_data; int32_t res; uint32_t flags; };
struct slu_getevents_arg { uint64_t sigmask; uint32_t sigmask_sz, pad; uint64_t ts; };
struct slu_timespec { int64_t tv_sec; long long tv_nsec; };
struct slu_sync_cancel {             /* io_uring_sync_cancel_reg (6.0+) */
    uint64_t addr;
    int32_t fd;
    uint32_t flags;
    struct slu_timespec timeout;
    uint8_t opcode, pad[7];
    uint64_t pad2[3];
};
_Static_assert(sizeof(struct slu_sync_cancel) == 64, "io_uring sync cancel");
#define SLU_REGISTER_SYNC_CANCEL 24

_Static_assert(sizeof(struct slu_sqe) == 64, "io_uring sqe");
_Static_assert(sizeof(struct slu_params) == 120, "io_uring params");

enum { SLU_OP_POLL_ADD = 6, SLU_OP_POLL_REMOVE = 7 };
#define SLU_FEAT_SINGLE_MMAP (1u << 0)
#define SLU_FEAT_NODROP (1u << 1)
#define SLU_FEAT_EXT_ARG (1u << 8)
#define SLU_SETUP_CQSIZE (1u << 3)
#define SLU_ENTER_GETEVENTS (1u << 0)
#define SLU_ENTER_EXT_ARG (1u << 3)
#define SLU_OFF_SQ_RING 0ULL
#define SLU_OFF_CQ_RING 0x8000000ULL
#define SLU_OFF_SQES 0x10000000ULL

#define UD_WAKE UINT64_MAX            /* the eventfd's poll */
#define UD_IGNORE (UINT64_MAX - 1)    /* completions of POLL_REMOVE requests */
#define SQ_ENTRIES 256u
#define CQ_ENTRIES 8192u

typedef struct ufd {
    uint64_t token;
    uint32_t seq;
    uint8_t pending;
} ufd;

typedef struct slang__uring {
    int fd, wakefd;
    void *sq_ring, *cq_ring;
    size_t sq_sz, cq_sz, sqes_sz;
    uint32_t *sq_head, *sq_tail, *sq_array, sq_mask, sq_entries;
    uint32_t *cq_head, *cq_tail, cq_mask;
    struct slu_sqe *sqes;
    struct slu_cqe *cqes;
    uint32_t to_submit;               /* SQEs queued, not yet passed to the kernel */
    slang__plat_mutex mu;             /* SQ ring, fds[], wake_armed */
    ufd *fds;
    int64_t nfds;
    int wake_armed;
} slang__uring;

static long sys_setup(unsigned entries, struct slu_params *p) { return syscall(__NR_io_uring_setup, entries, p); }
static long sys_register(int fd, unsigned op, void *arg, unsigned n) { return syscall(__NR_io_uring_register, fd, op, arg, n); }

/* Cancels the poll with user_data ud and waits until it is gone, so the
 * descriptor is no longer referenced by the ring when the caller closes it
 * (a pending poll holds a reference to the file: an asynchronous
 * POLL_REMOVE would let a closed listening socket accept a little longer).
 * Returns 0, or -errno (-ENOENT: nothing pending). */
static int sync_cancel(slang__uring *u, uint64_t ud) {
    struct slu_sync_cancel c;
    memset(&c, 0, sizeof c);
    c.addr = ud;
    c.fd = -1;
    c.timeout.tv_sec = -1;
    c.timeout.tv_nsec = -1;
    for (;;) {
        long r = sys_register(u->fd, SLU_REGISTER_SYNC_CANCEL, &c, 1);
        if (r >= 0) return 0;
        if (errno != EINTR) return -errno;
    }
}

static long sys_enter(int fd, unsigned submit, unsigned min, unsigned flags, void *arg, size_t argsz) {
    return syscall(__NR_io_uring_enter, fd, submit, min, flags, arg, argsz);
}

/* mu held: passes the queued SQEs to the kernel. */
static int flush(slang__uring *u) {
    while (u->to_submit) {
        long r = sys_enter(u->fd, u->to_submit, 0, 0, NULL, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EBUSY) {
                /* Completion queue backlog: let the poller reap, retry. */
                sched_yield();
                continue;
            }
            return errno;
        }
        u->to_submit -= (uint32_t)r;
    }
    return 0;
}

/* mu held: the next free SQE (zeroed). */
static struct slu_sqe *get_sqe(slang__uring *u) {
    uint32_t tail = *u->sq_tail;
    if (tail - __atomic_load_n(u->sq_head, __ATOMIC_ACQUIRE) >= u->sq_entries) {
        flush(u);
        while (tail - __atomic_load_n(u->sq_head, __ATOMIC_ACQUIRE) >= u->sq_entries) sched_yield();
    }
    uint32_t i = tail & u->sq_mask;
    struct slu_sqe *s = &u->sqes[i];
    memset(s, 0, sizeof *s);
    u->sq_array[i] = i;
    return s;
}

/* mu held: publishes the SQE taken last. */
static void push_sqe(slang__uring *u) {
    __atomic_store_n(u->sq_tail, *u->sq_tail + 1, __ATOMIC_RELEASE);
    u->to_submit++;
}

static void queue_poll(slang__uring *u, int fd, uint32_t events, uint64_t ud) {
    struct slu_sqe *s = get_sqe(u);
    s->opcode = SLU_OP_POLL_ADD;
    s->fd = fd;
    s->op_flags = events;
    s->user_data = ud;
    push_sqe(u);
}

static void queue_remove(slang__uring *u, uint64_t target) {
    struct slu_sqe *s = get_sqe(u);
    s->opcode = SLU_OP_POLL_REMOVE;
    s->fd = -1;
    s->addr = target;
    s->user_data = UD_IGNORE;
    push_sqe(u);
}

/* mu held: the per-fd state of fd (grown on demand); NULL if out of memory. */
static ufd *fd_state(slang__uring *u, int64_t fd) {
    if (fd < 0) return NULL;
    if (fd >= u->nfds) {
        int64_t n = u->nfds ? u->nfds : 1024;
        while (n <= fd) n *= 2;
        ufd *nf = realloc(u->fds, (size_t)n * sizeof *nf);
        if (!nf) return NULL;
        memset(nf + u->nfds, 0, (size_t)(n - u->nfds) * sizeof *nf);
        u->fds = nf;
        u->nfds = n;
    }
    return &u->fds[fd];
}

static uint64_t ud_of(int64_t fd, uint32_t seq) { return ((uint64_t)(uint32_t)fd << 32) | seq; }

void slang__uring_free(slang__uring *u);

slang__uring *slang__uring_new(void) {
    /* Opt-in (see the header): SLANG_IO_BACKEND=io_uring (or uring). */
    const char *be = getenv("SLANG_IO_BACKEND");
    if (!be || (strcmp(be, "io_uring") != 0 && strcmp(be, "uring") != 0)) return NULL;
    slang__uring *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->fd = u->wakefd = -1;
    struct slu_params p;
    memset(&p, 0, sizeof p);
    p.flags = SLU_SETUP_CQSIZE;
    p.cq_entries = CQ_ENTRIES;
    long fd = sys_setup(SQ_ENTRIES, &p);
    if (fd < 0) { free(u); return NULL; }
    u->fd = (int)fd;
    if ((p.features & (SLU_FEAT_EXT_ARG | SLU_FEAT_NODROP | SLU_FEAT_SINGLE_MMAP)) != (SLU_FEAT_EXT_ARG | SLU_FEAT_NODROP | SLU_FEAT_SINGLE_MMAP)) {
        slang__uring_free(u);
        return NULL;
    }
    u->sq_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    u->cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct slu_cqe);
    if (u->cq_sz > u->sq_sz) u->sq_sz = u->cq_sz;
    u->cq_sz = u->sq_sz;   /* SINGLE_MMAP: one mapping for both rings */
    u->sq_ring = mmap(NULL, u->sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, u->fd, SLU_OFF_SQ_RING);
    if (u->sq_ring == MAP_FAILED) { u->sq_ring = NULL; slang__uring_free(u); return NULL; }
    u->cq_ring = u->sq_ring;
    u->sqes_sz = p.sq_entries * sizeof(struct slu_sqe);
    u->sqes = mmap(NULL, u->sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, u->fd, SLU_OFF_SQES);
    if (u->sqes == MAP_FAILED) { u->sqes = NULL; slang__uring_free(u); return NULL; }
    char *sq = u->sq_ring, *cq = u->cq_ring;
    u->sq_head = (uint32_t *)(sq + p.sq_off.head);
    u->sq_tail = (uint32_t *)(sq + p.sq_off.tail);
    u->sq_mask = *(uint32_t *)(sq + p.sq_off.ring_mask);
    u->sq_entries = *(uint32_t *)(sq + p.sq_off.ring_entries);
    u->sq_array = (uint32_t *)(sq + p.sq_off.array);
    u->cq_head = (uint32_t *)(cq + p.cq_off.head);
    u->cq_tail = (uint32_t *)(cq + p.cq_off.tail);
    u->cq_mask = *(uint32_t *)(cq + p.cq_off.ring_mask);
    u->cqes = (struct slu_cqe *)(cq + p.cq_off.cqes);
    /* Synchronous cancellation (Linux 6.0+) is required: probe it with a
     * request that does not exist. */
    if (sync_cancel(u, UD_IGNORE - 1) != -ENOENT) { slang__uring_free(u); return NULL; }
    u->wakefd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    slang__plat_mutex_init(&u->mu);
    if (u->wakefd < 0) { slang__uring_free(u); return NULL; }
    queue_poll(u, u->wakefd, POLLIN, UD_WAKE);
    u->wake_armed = 1;
    if (flush(u) != 0) { slang__uring_free(u); return NULL; }
    return u;
}

void slang__uring_free(slang__uring *u) {
    if (!u) return;
    if (u->sqes) munmap(u->sqes, u->sqes_sz);
    if (u->sq_ring) munmap(u->sq_ring, u->sq_sz);
    if (u->fd >= 0) close(u->fd);
    if (u->wakefd >= 0) {
        close(u->wakefd);
        slang__plat_mutex_destroy(&u->mu);
    }
    free(u->fds);
    free(u);
}

int slang__uring_arm(slang__uring *u, int64_t fd, int32_t interest, uint64_t token, bool registered) {
    if (!registered) {
        /* What epoll_ctl(ADD) reports at once: a closed descriptor, or one
         * that cannot be polled (regular files and directories are always
         * ready). */
        struct stat st;
        if (fstat((int)fd, &st) != 0) return errno;
        if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) return SLANG_PLAT_ENOTPOLLABLE;
    }
    uint32_t ev = 0;
    if (interest & SLANG_PLAT_READ) ev |= POLLIN | POLLRDHUP;
    if (interest & SLANG_PLAT_WRITE) ev |= POLLOUT;
    slang__plat_mutex_lock(&u->mu);
    ufd *e = fd_state(u, fd);
    if (!e) {
        slang__plat_mutex_unlock(&u->mu);
        return fd < 0 ? EBADF : ENOMEM;
    }
    if (e->pending) queue_remove(u, ud_of(fd, e->seq));
    e->pending = 0;
    e->seq++;
    if (ev) {
        e->token = token;
        e->pending = 1;
        queue_poll(u, (int)fd, ev, ud_of(fd, e->seq));
    }
    int rc = flush(u);
    slang__plat_mutex_unlock(&u->mu);
    return rc;
}

void slang__uring_remove(slang__uring *u, int64_t fd) {
    slang__plat_mutex_lock(&u->mu);
    ufd *e = fd < u->nfds ? fd_state(u, fd) : NULL;
    if (e && e->pending) sync_cancel(u, ud_of(fd, e->seq));
    if (e) {
        e->pending = 0;
        e->seq++;
    }
    slang__plat_mutex_unlock(&u->mu);
}

int slang__uring_wait(slang__uring *u, slang__plat_event *evs, int max, int64_t timeout_ms) {
    if (max < 1) return 0;
    if (__atomic_load_n(u->cq_head, __ATOMIC_RELAXED) == __atomic_load_n(u->cq_tail, __ATOMIC_ACQUIRE) && timeout_ms != 0) {
        struct slu_timespec ts = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000LL };
        struct slu_getevents_arg arg = { 0, 0, 0, (uint64_t)(uintptr_t)&ts };
        if (timeout_ms < 0) arg.ts = 0;   /* no timeout */
        long r = sys_enter(u->fd, 0, 1, SLU_ENTER_GETEVENTS | SLU_ENTER_EXT_ARG, &arg, sizeof arg);
        (void)r;   /* ETIME, EINTR: report what is there (possibly nothing) */
    }
    int out = 0, rearm_wake = 0;
    slang__plat_mutex_lock(&u->mu);
    uint32_t head = *u->cq_head;
    uint32_t tail = __atomic_load_n(u->cq_tail, __ATOMIC_ACQUIRE);
    while (head != tail && out < max) {
        struct slu_cqe *c = &u->cqes[head & u->cq_mask];
        uint64_t ud = c->user_data;
        int32_t res = c->res;
        head++;
        if (ud == UD_IGNORE) continue;
        if (ud == UD_WAKE) {
            uint64_t v;
            while (read(u->wakefd, &v, sizeof v) > 0) {}
            u->wake_armed = 0;
            rearm_wake = 1;
            evs[out].token = SLANG_PLAT_WAKE_TOKEN;
            evs[out].events = 0;
            out++;
            continue;
        }
        int64_t fd = (int64_t)(ud >> 32);
        uint32_t seq = (uint32_t)ud;
        ufd *e = fd < u->nfds ? &u->fds[fd] : NULL;
        if (!e || !e->pending || e->seq != seq) continue;   /* replaced or removed */
        e->pending = 0;
        if (res == -ECANCELED) continue;
        int32_t ev = 0;
        if (res < 0) {
            ev = SLANG_PLAT_ERROR | SLANG_PLAT_READ | SLANG_PLAT_WRITE;
        } else {
            if (res & (POLLIN | POLLRDHUP)) ev |= SLANG_PLAT_READ;
            if (res & POLLOUT) ev |= SLANG_PLAT_WRITE;
            if (res & (POLLERR | POLLHUP | POLLNVAL)) ev |= SLANG_PLAT_ERROR | SLANG_PLAT_READ | SLANG_PLAT_WRITE;
        }
        evs[out].token = e->token;
        evs[out].events = ev;
        out++;
    }
    __atomic_store_n(u->cq_head, head, __ATOMIC_RELEASE);
    if (rearm_wake && !u->wake_armed) {
        queue_poll(u, u->wakefd, POLLIN, UD_WAKE);
        u->wake_armed = 1;
        flush(u);
    }
    slang__plat_mutex_unlock(&u->mu);
    return out;
}

void slang__uring_wake(slang__uring *u) {
    uint64_t one = 1;
    for (;;) {
        ssize_t r = write(u->wakefd, &one, sizeof one);
        if (r >= 0 || errno != EINTR) break;  /* EAGAIN: counter saturated, already pending */
    }
}

#else
/* Not Linux: no io_uring. */
typedef int slang__poll_uring_unused;
#endif
