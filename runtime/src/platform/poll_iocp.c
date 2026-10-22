/* poll_iocp.c - readiness poller on Windows (platform.h): one I/O
 * completion port per poller (= per task_io.c shard, like the epoll
 * shards) with readiness learnt from IOCTL_AFD_POLL operations.
 *
 * Design (docs/PLATFORMS.md, "IOCP"). The platform interface is readiness
 * based (arm one-shot, report, the caller retries its non-blocking
 * recv/send), IOCP is completion based. The adapter keeps the readiness
 * contract, so task_io.c, std_net.c and the socket code are unchanged:
 *
 *  - every poller opens its own handle to the AFD driver (\Device\Afd, the
 *    kernel side of Winsock) and associates it with its completion port;
 *  - arming a socket issues ONE IOCTL_AFD_POLL on that handle for the
 *    socket's base handle with the AFD events of the interest (receive /
 *    accept / disconnect for READ, send for WRITE, abort / connect-fail /
 *    local-close for both). The driver completes it as soon as the socket
 *    is in that state (level triggered: readiness already there completes
 *    at once), and the completion lands on the port with the per-socket
 *    record as its context. That completion is the one-shot report; the
 *    record stays registered but disarmed until the next arm.
 *    This is how wepoll, mio/tokio and libuv's uv_poll do it; zero-byte
 *    WSARecv/WSASend were rejected because a zero-byte WSASend completes
 *    immediately whether or not the send buffer has room (a busy loop on a
 *    full socket) and a zero-byte WSARecv does not report accept().
 *  - changing the interest while a poll is pending (a writer joins a
 *    reader) cancels it (CancelIoEx); the cancelled completion re-issues
 *    the poll with the current interest. remove() cancels and frees the
 *    record when its last completion arrives (the kernel owns the
 *    IO_STATUS_BLOCK until then).
 *  - wake-ups are PostQueuedCompletionStatus packets with a reserved key
 *    (the eventfd of poll_epoll.c), coalesced by a flag; waits are
 *    GetQueuedCompletionStatusEx batches. Timeouts stay in task_io.c's
 *    deadline heaps (the wait timeout), as on Linux.
 *
 * Child-process pipes (proc_win32.c: overlapped named pipes) are armed
 * too: readability is a zero-byte overlapped ReadFile on the pipe, which
 * completes on the port when data (or EOF) arrives; writability is the
 * completion of the pipe's write in flight (proc_win32.c issues at most
 * one), or at once if there is none. Anything else cannot be armed
 * (ZELL_PLAT_ENOTPOLLABLE: files are always ready).
 * When \Device\Afd cannot be opened (very old Windows, a sandbox), or with
 * ZELL_POLLER=wsapoll, the poller is the WSAPoll one of poll_wsapoll.c. */
#include "platform.h"

#if defined(_WIN32)

#include "win32_util.h"

#include <mswsock.h>
#include <winternl.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---- the AFD interface (not in the SDK headers; layout as wepoll/mio) ---- */

#define ZELL_IOCTL_AFD_POLL 0x00012024
enum {
    AFD_POLL_RECEIVE = 0x0001,
    AFD_POLL_RECEIVE_EXPEDITED = 0x0002,
    AFD_POLL_SEND = 0x0004,
    AFD_POLL_DISCONNECT = 0x0008,
    AFD_POLL_ABORT = 0x0010,
    AFD_POLL_LOCAL_CLOSE = 0x0020,
    AFD_POLL_ACCEPT = 0x0080,
    AFD_POLL_CONNECT_FAIL = 0x0100,
};

typedef struct afd_poll_handle_info {
    HANDLE handle;
    ULONG events;
    NTSTATUS status;
} afd_poll_handle_info;

typedef struct afd_poll_info {
    LARGE_INTEGER timeout;
    ULONG count;
    ULONG exclusive;
    afd_poll_handle_info handles[1];
} afd_poll_info;

#ifndef STATUS_PENDING
#define STATUS_PENDING ((NTSTATUS)0x00000103L)
#endif
#define ZELL_STATUS_CANCELLED ((NTSTATUS)0xC0000120L)

typedef NTSTATUS(NTAPI *nt_create_file_fn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG,
                                           ULONG, PVOID, ULONG);
typedef NTSTATUS(NTAPI *nt_ioctl_fn)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG, PVOID, ULONG);
typedef ULONG(NTAPI *rtl_status_fn)(NTSTATUS);

static nt_create_file_fn g_nt_create;
static nt_ioctl_fn g_nt_ioctl;
static rtl_status_fn g_nt_status;
static zell__plat_once g_nt_once = ZELL_PLAT_ONCE_INIT;

static void nt_load(void) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return;
    g_nt_create = (nt_create_file_fn)(void *)GetProcAddress(nt, "NtCreateFile");
    g_nt_ioctl = (nt_ioctl_fn)(void *)GetProcAddress(nt, "NtDeviceIoControlFile");
    g_nt_status = (rtl_status_fn)(void *)GetProcAddress(nt, "RtlNtStatusToDosError");
}

/* A handle to the AFD driver for poll requests; NULL if unavailable. */
static HANDLE afd_open(void) {
    zell__plat_once_run(&g_nt_once, nt_load);
    if (!g_nt_create || !g_nt_ioctl) return NULL;
    static const wchar_t *names[] = { L"\\Device\\Afd\\Zell", L"\\Device\\Afd" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        UNICODE_STRING us;
        us.Buffer = (PWSTR)names[i];
        us.Length = (USHORT)(wcslen(names[i]) * sizeof(wchar_t));
        us.MaximumLength = us.Length;
        OBJECT_ATTRIBUTES oa;
        memset(&oa, 0, sizeof oa);
        oa.Length = sizeof oa;
        oa.ObjectName = &us;
        IO_STATUS_BLOCK iosb;
        HANDLE h = NULL;
        NTSTATUS st = g_nt_create(&h, SYNCHRONIZE, &oa, &iosb, NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 1 /* FILE_OPEN */, 0, NULL, 0);
        if (st == 0 && h) return h;
    }
    return NULL;
}

/* ---- per-socket records ---- */

enum { REC_SOCK, REC_PIPE_READ, REC_PIPE_WRITE };

typedef struct sock_rec {
    union {
        IO_STATUS_BLOCK iosb; /* AFD poll; first: the completion's lpOverlapped points here */
        OVERLAPPED ov;        /* pipe zero-byte read, or a posted readiness packet */
    } u;
    afd_poll_info info;      /* in/out buffer of the pending poll */
    int kind;                /* REC_* */
    HANDLE pipe;             /* REC_PIPE_*: the parent's pipe handle */
    bool wait_write;         /* REC_PIPE_WRITE: pending = waiting for the write's packet */
    SOCKET s;                /* the descriptor as the runtime knows it */
    SOCKET base;             /* SIO_BASE_HANDLE (bypasses layered providers) */
    uint64_t token;
    int32_t interest;        /* armed interest, 0 = disarmed */
    int32_t polled;          /* interest of the pending poll */
    bool pending;            /* an IOCTL_AFD_POLL is in flight */
    bool cancelling;         /* ... and CancelIoEx was called for it */
    bool removed;            /* remove() ran: free at the last completion */
    struct sock_rec *next;   /* hash chain */
} sock_rec;

#define WAKE_KEY ((ULONG_PTR)1)
#define PIPE_KEY ((ULONG_PTR)2)
#define ZELL_STATUS_PIPE_BROKEN ((NTSTATUS)0xC000014BL)

struct zell__plat_poller {
    zell__w32_wsapoll *fallback;  /* non-NULL: the WSAPoll backend */
    HANDLE port;
    HANDLE afd;
    zell__plat_mutex mu;
    sock_rec **tab;                /* hash by SOCKET value */
    size_t ntab, count;
    int64_t inflight;              /* polls in flight (under mu) */
    volatile LONG woken;
};

static size_t hash_sock(SOCKET s, size_t n) {
    uint64_t x = (uint64_t)s >> 2;  /* handles are multiples of 4 */
    x *= 0x9E3779B97F4A7C15ull;
    return (size_t)(x >> 20) & (n - 1);
}

static sock_rec **find_slot(zell__plat_poller *p, SOCKET s) {
    sock_rec **pp = &p->tab[hash_sock(s, p->ntab)];
    while (*pp && (*pp)->s != s) pp = &(*pp)->next;
    return pp;
}

static bool grow(zell__plat_poller *p) {
    size_t nn = p->ntab * 2;
    sock_rec **nt = calloc(nn, sizeof *nt);
    if (!nt) return false;
    for (size_t i = 0; i < p->ntab; i++) {
        sock_rec *r = p->tab[i];
        while (r) {
            sock_rec *next = r->next;
            size_t h = hash_sock(r->s, nn);
            r->next = nt[h];
            nt[h] = r;
            r = next;
        }
    }
    free(p->tab);
    p->tab = nt;
    p->ntab = nn;
    return true;
}

static int status_errno(NTSTATUS st) {
    DWORD e = g_nt_status ? g_nt_status(st) : ERROR_INVALID_HANDLE;
    if (e == ERROR_INVALID_HANDLE || e == ERROR_NOT_FOUND) return EBADF;
    return zell__w32_errno(e);
}

/* mu held: a readiness packet for r posted by hand (status: its result). */
static void post_ready(zell__plat_poller *p, sock_rec *r, NTSTATUS status) {
    memset(&r->u.ov, 0, sizeof r->u.ov);
    r->u.ov.Internal = (ULONG_PTR)status;
    r->pending = true;
    r->cancelling = false;
    r->polled = r->interest;
    p->inflight++;
    PostQueuedCompletionStatus(p->port, 0, PIPE_KEY, &r->u.ov);
}

/* mu held: the pipe variants of submit. */
static int submit_pipe(zell__plat_poller *p, sock_rec *r) {
    if (r->kind == REC_PIPE_WRITE) {
        if (zell__w32_pipe_write_busy((int64_t)r->s)) {
            /* the write's own completion packet reports it (no op of ours) */
            r->pending = true;
            r->wait_write = true;
            r->polled = r->interest;
        } else {
            post_ready(p, r, 0);
        }
        return 0;
    }
    static char zero;
    memset(&r->u.ov, 0, sizeof r->u.ov);
    if (ReadFile(r->pipe, &zero, 0, NULL, &r->u.ov) || GetLastError() == ERROR_IO_PENDING) {
        r->pending = true;  /* a packet comes either way (the handle is on the port) */
        r->cancelling = false;
        r->polled = r->interest;
        p->inflight++;
        return 0;
    }
    DWORD e = GetLastError();
    if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED || e == ERROR_NO_DATA) {
        post_ready(p, r, ZELL_STATUS_PIPE_BROKEN);  /* EOF: readable */
        return 0;
    }
    return zell__w32_errno(e);
}

/* mu held: issues the poll for r->interest. 0 or an errno. */
static int submit(zell__plat_poller *p, sock_rec *r) {
    if (r->kind != REC_SOCK) return submit_pipe(p, r);
    ULONG ev = AFD_POLL_ABORT | AFD_POLL_LOCAL_CLOSE | AFD_POLL_CONNECT_FAIL;
    if (r->interest & ZELL_PLAT_READ) ev |= AFD_POLL_RECEIVE | AFD_POLL_ACCEPT | AFD_POLL_DISCONNECT;
    if (r->interest & ZELL_PLAT_WRITE) ev |= AFD_POLL_SEND;
    r->info.timeout.QuadPart = INT64_MAX;
    r->info.count = 1;
    r->info.exclusive = FALSE;
    r->info.handles[0].handle = (HANDLE)r->base;
    r->info.handles[0].events = ev;
    r->info.handles[0].status = 0;
    r->u.iosb.Status = STATUS_PENDING;
    r->u.iosb.Information = 0;
    NTSTATUS st = g_nt_ioctl(p->afd, NULL, NULL, &r->u.iosb, &r->u.iosb, ZELL_IOCTL_AFD_POLL, &r->info, sizeof r->info, &r->info,
                             sizeof r->info);
    if (st != 0 && st != STATUS_PENDING) return status_errno(st);
    /* success or pending: the completion is queued to the port either way */
    r->pending = true;
    r->cancelling = false;
    r->polled = r->interest;
    p->inflight++;
    return 0;
}

/* mu held: cancels r's pending operation; true if r has none left (a
 * record waiting for a pipe write owns no operation). */
static bool cancel(zell__plat_poller *p, sock_rec *r) {
    if (!r->pending) return true;
    if (r->wait_write) {
        r->pending = false;
        r->wait_write = false;
        return true;
    }
    if (r->cancelling) return false;
    r->cancelling = true;
    if (r->kind != REC_SOCK) {
        CancelIoEx(r->pipe, &r->u.ov);  /* a posted packet cannot be cancelled: it just arrives */
        return false;
    }
    /* the IO_STATUS_BLOCK doubles as the OVERLAPPED of the request */
    if (!CancelIoEx(p->afd, (LPOVERLAPPED)&r->u.iosb) && GetLastError() == ERROR_NOT_FOUND) {
        /* already completed: its packet is on the way */
    }
    return false;
}

/* ---- the interface ---- */

zell__plat_poller *zell__plat_poller_new(void) {
    zell__w32_wsa_init();
    zell__plat_poller *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    const char *force = getenv("ZELL_POLLER");
    HANDLE afd = (force && strcmp(force, "wsapoll") == 0) ? NULL : afd_open();
    HANDLE port = afd ? CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1) : NULL;
    if (port && CreateIoCompletionPort(afd, port, 0, 0) != port) {
        CloseHandle(port);
        port = NULL;
    }
    if (!port) {
        if (afd) CloseHandle(afd);
        p->fallback = zell__w32_wsapoll_new();
        if (!p->fallback) { free(p); return NULL; }
        return p;
    }
    /* completions of polls that succeed at once are still queued (the
     * default), so every request has exactly one packet */
    p->afd = afd;
    p->port = port;
    p->ntab = 64;
    p->tab = calloc(p->ntab, sizeof *p->tab);
    if (!p->tab) {
        CloseHandle(afd);
        CloseHandle(port);
        free(p);
        return NULL;
    }
    zell__plat_mutex_init(&p->mu);
    return p;
}

/* Drains completions (mu not held) until no poll is in flight or `ms`
 * passed; frees the records of removed sockets as they complete. */
static void drain(zell__plat_poller *p, DWORD ms) {
    DWORD start = GetTickCount();
    for (;;) {
        zell__plat_mutex_lock(&p->mu);
        int64_t left = p->inflight;
        zell__plat_mutex_unlock(&p->mu);
        DWORD spent = GetTickCount() - start;
        if (left == 0 || spent >= ms) return;
        OVERLAPPED_ENTRY e[64];
        ULONG n = 0;
        if (!GetQueuedCompletionStatusEx(p->port, e, 64, &n, ms - spent, FALSE)) return;
        zell__plat_mutex_lock(&p->mu);
        for (ULONG i = 0; i < n; i++) {
            int64_t wfd;
            if (e[i].lpCompletionKey == WAKE_KEY || !e[i].lpOverlapped || zell__w32_pipe_write_ov(e[i].lpOverlapped, &wfd)) continue;
            sock_rec *r = (sock_rec *)(void *)e[i].lpOverlapped;
            r->pending = false;
            p->inflight--;
            if (r->removed) free(r);
        }
        zell__plat_mutex_unlock(&p->mu);
    }
}

void zell__plat_poller_free(zell__plat_poller *p) {
    if (!p) return;
    if (p->fallback) {
        zell__w32_wsapoll_free(p->fallback);
        free(p);
        return;
    }
    zell__plat_mutex_lock(&p->mu);
    for (size_t i = 0; i < p->ntab; i++) {
        sock_rec *r = p->tab[i];
        while (r) {
            sock_rec *next = r->next;
            r->removed = true;
            if (cancel(p, r)) free(r);
            r = next;
        }
        p->tab[i] = NULL;
    }
    zell__plat_mutex_unlock(&p->mu);
    drain(p, 2000);
    /* a poll that never completed keeps its record (the kernel may still
     * write to it): leaked on purpose */
    CloseHandle(p->afd);
    CloseHandle(p->port);
    free(p->tab);
    zell__plat_mutex_destroy(&p->mu);
    free(p);
}

int zell__plat_poller_arm(zell__plat_poller *p, int64_t fd, int32_t interest, uint64_t token, bool registered) {
    if (p->fallback) return zell__w32_wsapoll_arm(p->fallback, fd, interest, token, registered);
    SOCKET s = (SOCKET)fd;
    interest &= ZELL_PLAT_READ | ZELL_PLAT_WRITE;
    zell__plat_mutex_lock(&p->mu);
    sock_rec **slot = find_slot(p, s);
    sock_rec *r = *slot;
    if (!r) {
        zell__plat_mutex_unlock(&p->mu);
        HANDLE ph = NULL;
        bool pw = false;
        int kind = REC_SOCK;
        SOCKET base = s;
        if (zell__w32_pipe_of(fd, &ph, &pw)) {
            /* a child-process pipe: its completions come to this port */
            if (CreateIoCompletionPort(ph, p->port, PIPE_KEY, 0) != p->port) return EBADF;
            kind = pw ? REC_PIPE_WRITE : REC_PIPE_READ;
        } else {
            int type = 0, tl = sizeof type;
            if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &tl) != 0)
                return WSAGetLastError() == WSAENOTSOCK ? ZELL_PLAT_ENOTPOLLABLE : EBADF;
            DWORD got = 0;
            if (WSAIoctl(s, SIO_BASE_HANDLE, NULL, 0, &base, sizeof base, &got, NULL, NULL) != 0 || base == INVALID_SOCKET) base = s;
        }
        r = calloc(1, sizeof *r);
        if (!r) return ENOMEM;
        r->s = s;
        r->base = base;
        r->kind = kind;
        r->pipe = ph;
        zell__plat_mutex_lock(&p->mu);
        slot = find_slot(p, s);
        if (*slot) {  /* armed concurrently: use that one */
            free(r);
            r = *slot;
        } else {
            *slot = r;
            if (++p->count > p->ntab && !grow(p)) { /* keep the longer chains */ }
        }
    }
    r->token = token;
    r->interest = interest;
    int rc = 0;
    if (interest == 0) cancel(p, r);
    else if (!r->pending) rc = submit(p, r);
    else if ((interest & ~r->polled) != 0 && cancel(p, r)) rc = submit(p, r);  /* else re-issued by its cancelled completion */
    if (rc) r->interest = 0;
    zell__plat_mutex_unlock(&p->mu);
    return rc;
}

void zell__plat_poller_remove(zell__plat_poller *p, int64_t fd) {
    if (p->fallback) { zell__w32_wsapoll_remove(p->fallback, fd); return; }
    zell__plat_mutex_lock(&p->mu);
    sock_rec **slot = find_slot(p, (SOCKET)fd);
    sock_rec *r = *slot;
    if (r) {
        *slot = r->next;
        p->count--;
        r->removed = true;
        r->interest = 0;
        if (cancel(p, r)) free(r);
    }
    zell__plat_mutex_unlock(&p->mu);
}

static int32_t events_of(const sock_rec *r) {
    if (r->kind == REC_PIPE_WRITE) return ZELL_PLAT_WRITE | (r->u.ov.Internal ? ZELL_PLAT_ERROR : 0);
    if (r->kind == REC_PIPE_READ) return ZELL_PLAT_READ | (r->u.ov.Internal ? ZELL_PLAT_ERROR : 0);  /* data, or EOF */
    if (r->u.iosb.Status != 0) {
        /* the poll itself failed: the socket was closed or is invalid */
        return ZELL_PLAT_ERROR | ZELL_PLAT_READ | ZELL_PLAT_WRITE;
    }
    if (r->info.count < 1) return 0;
    ULONG a = r->info.handles[0].events;
    int32_t ev = 0;
    if (a & (AFD_POLL_RECEIVE | AFD_POLL_RECEIVE_EXPEDITED | AFD_POLL_ACCEPT | AFD_POLL_DISCONNECT)) ev |= ZELL_PLAT_READ;
    if (a & AFD_POLL_SEND) ev |= ZELL_PLAT_WRITE;
    if (a & AFD_POLL_DISCONNECT) ev |= ZELL_PLAT_WRITE & r->interest;
    if (a & (AFD_POLL_ABORT | AFD_POLL_CONNECT_FAIL | AFD_POLL_LOCAL_CLOSE))
        ev |= ZELL_PLAT_ERROR | ZELL_PLAT_READ | ZELL_PLAT_WRITE;
    return ev;
}

int zell__plat_poller_wait(zell__plat_poller *p, zell__plat_event *evs, int max, int64_t timeout_ms) {
    if (p->fallback) return zell__w32_wsapoll_wait(p->fallback, evs, max, timeout_ms);
    OVERLAPPED_ENTRY ent[256];
    if (max > 256) max = 256;
    if (max < 1) return 0;
    DWORD t = timeout_ms < 0 ? INFINITE : timeout_ms >= 0x7fffffff ? 0x7ffffffe : (DWORD)timeout_ms;
    ULONG n = 0;
    if (!GetQueuedCompletionStatusEx(p->port, ent, (ULONG)max, &n, t, FALSE)) return 0;  /* WAIT_TIMEOUT */
    int out = 0;
    zell__plat_mutex_lock(&p->mu);
    for (ULONG i = 0; i < n; i++) {
        if (ent[i].lpCompletionKey == WAKE_KEY) {
            InterlockedExchange(&p->woken, 0);
            evs[out].token = ZELL_PLAT_WAKE_TOKEN;
            evs[out++].events = 0;
            continue;
        }
        int64_t wfd;
        if (ent[i].lpOverlapped && zell__w32_pipe_write_ov(ent[i].lpOverlapped, &wfd)) {
            /* a child-pipe write completed: report the writer waiting for it */
            sock_rec *w = *find_slot(p, (SOCKET)wfd);
            if (w && w->wait_write && w->pending && w->interest) {
                w->pending = false;
                w->wait_write = false;
                w->interest = 0;
                evs[out].token = w->token;
                evs[out++].events = ZELL_PLAT_WRITE;
            }
            continue;
        }
        sock_rec *r = (sock_rec *)(void *)ent[i].lpOverlapped;
        if (!r) continue;
        r->pending = false;
        p->inflight--;
        if (r->removed) {
            free(r);
            continue;
        }
        bool was_cancelled = r->cancelling;
        r->cancelling = false;
        if (was_cancelled && r->u.iosb.Status == ZELL_STATUS_CANCELLED) {
            /* cancelled for an interest change: ask again */
            if (r->interest && submit(p, r) != 0) {
                evs[out].token = r->token;
                evs[out++].events = ZELL_PLAT_ERROR | ZELL_PLAT_READ | ZELL_PLAT_WRITE;
                r->interest = 0;
            }
            continue;
        }
        if (!r->interest) continue;  /* disarmed meanwhile */
        int32_t ev = events_of(r);
        if ((ev & (r->interest | ZELL_PLAT_ERROR)) == 0) {
            /* nothing the waiters asked for (a timeout, or a cancelled poll
             * whose interest now differs): poll again */
            if (submit(p, r) != 0) {
                evs[out].token = r->token;
                evs[out++].events = ZELL_PLAT_ERROR | ZELL_PLAT_READ | ZELL_PLAT_WRITE;
                r->interest = 0;
            }
            continue;
        }
        r->interest = 0;  /* one-shot */
        evs[out].token = r->token;
        evs[out++].events = ev;
    }
    zell__plat_mutex_unlock(&p->mu);
    return out;
}

void zell__plat_poller_wake(zell__plat_poller *p) {
    if (p->fallback) { zell__w32_wsapoll_wake(p->fallback); return; }
    if (!InterlockedExchange(&p->woken, 1)) PostQueuedCompletionStatus(p->port, 0, WAKE_KEY, NULL);
}

static zell__plat_once g_probe_once = ZELL_PLAT_ONCE_INIT;
static bool g_probe_iocp;

static void probe(void) {
    const char *force = getenv("ZELL_POLLER");
    if (force && strcmp(force, "wsapoll") == 0) return;
    HANDLE afd = afd_open();
    if (!afd) return;
    CloseHandle(afd);
    g_probe_iocp = true;
}

/* True if pollers created now are IOCP ones (they can wait on pipes). */
bool zell__w32_poller_waits_pipes(void) {
    zell__plat_once_run(&g_probe_once, probe);
    return g_probe_iocp;
}

/* Which backend a poller uses ("iocp" or "wsapoll"); for tests. */
const char *zell__plat_poller_backend(zell__plat_poller *p) { return p->fallback ? "wsapoll" : "iocp"; }

#else
typedef int zell__poll_iocp_unused;
#endif

