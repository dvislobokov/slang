/* proc_win32.c - Windows child processes for the platform layer
 * (platform.h): CreateProcessW with a quoted UTF-16 command line and
 * environment block. Process handles are kept in a table by pid until
 * reaped.
 *
 * Piped streams, with the IOCP poller (poll_iocp.c, the default): the
 * parent's end is an overlapped named pipe (\\.\pipe\zell-<pid>-<n>, one
 * instance, local clients only), the child's end a synchronous handle to
 * it. The parent's descriptor is PIPE_FD_BASE + a slot of the pipe table
 * (never a socket, CRT or HANDLE value). The poller waits on it directly:
 * readability is a zero-byte overlapped ReadFile completing on the shard's
 * port (libuv's "zero read"), writability the completion of the one write
 * in flight. Reads are non-blocking (PeekNamedPipe, then a ReadFile of what
 * is there); a write hands up to 64 KiB to an overlapped WriteFile and
 * returns, the next one waits (EAGAIN) until it has completed. No thread
 * per pipe.
 *
 * With the WSAPoll fallback poller (sockets only), a piped stream is
 * relayed instead: the child gets an anonymous pipe, a relay thread copies
 * between the parent's pipe end and one end of a loopback socket pair,
 * and the parent works with the other, non-blocking socket end. EOF
 * propagates both ways (the relay shuts its socket down when the child
 * closes its end, and closes the pipe when the parent closes its socket). */
#include "platform.h"

#if defined(_WIN32)

#include "win32_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- handles of live children ----------------------------------------------- */

typedef struct child_rec { int64_t pid; HANDLE h; int killed; struct child_rec *next; } child_rec;
static zell__plat_mutex g_mu = ZELL_PLAT_MUTEX_INIT;
static zell__plat_mutex g_spawn_mu = ZELL_PLAT_MUTEX_INIT;  /* one spawn at a time: inheritable handles */
static child_rec *g_children;

static child_rec *find(int64_t pid) {
    for (child_rec *c = g_children; c; c = c->next)
        if (c->pid == pid) return c;
    return NULL;
}

/* ---- command line --------------------------------------------------------------- */

/* Appends arg quoted by the rules of CommandLineToArgvW / the C runtime. */
static void quote(char **buf, size_t *len, size_t *cap, const char *arg) {
    size_t need = *len + strlen(arg) * 2 + 4;
    if (need > *cap) {
        *cap = need * 2;
        *buf = realloc(*buf, *cap);
        if (!*buf) return;
    }
    char *b = *buf;
    size_t n = *len;
    if (n) b[n++] = ' ';
    bool q = !*arg || strpbrk(arg, " \t\n\v\"") != NULL;
    if (!q) {
        size_t al = strlen(arg);
        memcpy(b + n, arg, al);
        *len = n + al;
        b[*len] = 0;
        return;
    }
    b[n++] = '"';
    for (const char *p = arg;; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (!*p) {
            for (size_t i = 0; i < bs * 2; i++) b[n++] = '\\';
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < bs * 2 + 1; i++) b[n++] = '\\';
            b[n++] = '"';
        } else {
            for (size_t i = 0; i < bs; i++) b[n++] = '\\';
            b[n++] = *p;
        }
    }
    b[n++] = '"';
    b[n] = 0;
    *len = n;
}

static wchar_t *command_line(char *const *argv) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (char *const *a = argv; *a; a++) {
        quote(&buf, &len, &cap, *a);
        if (!buf) return NULL;
    }
    wchar_t *w = zell__w32_wide(buf ? buf : "");
    free(buf);
    return w;
}

/* UTF-16 environment block: "K=V\0K=V\0\0". */
static wchar_t *env_block(char *const *envp) {
    size_t total = 1;
    for (char *const *e = envp; *e; e++) {
        int n = MultiByteToWideChar(CP_UTF8, 0, *e, -1, NULL, 0);
        if (n <= 0) return NULL;
        total += (size_t)n;
    }
    wchar_t *b = malloc(total * sizeof *b), *p = b;
    if (!b) return NULL;
    for (char *const *e = envp; *e; e++) p += MultiByteToWideChar(CP_UTF8, 0, *e, -1, p, (int)(total - (size_t)(p - b)));
    *p = 0;
    if (p == b) p[1] = 0;  /* an empty block still needs two NULs */
    return b;
}

/* ---- relays ------------------------------------------------------------------------ */

typedef struct relay { HANDLE pipe; SOCKET sock; bool to_child; } relay;

static void *relay_main(void *arg) {
    relay r = *(relay *)arg;
    free(arg);
    char buf[16384];
    if (r.to_child) {
        for (;;) {
            int n = recv(r.sock, buf, sizeof buf, 0);
            if (n <= 0) break;
            DWORD off = 0, w = 0;
            while (off < (DWORD)n && WriteFile(r.pipe, buf + off, (DWORD)n - off, &w, NULL)) off += w;
            if (off < (DWORD)n) break;  /* the child closed its stdin */
        }
    } else {
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(r.pipe, buf, sizeof buf, &got, NULL) || got == 0) break;  /* ERROR_BROKEN_PIPE: EOF */
            int off = 0;
            while (off < (int)got) {
                int s = send(r.sock, buf + off, (int)got - off, 0);
                if (s <= 0) break;
                off += s;
            }
            if (off < (int)got) break;  /* the parent closed its end */
        }
        shutdown(r.sock, SD_SEND);
    }
    CloseHandle(r.pipe);
    closesocket(r.sock);
    return NULL;
}

/* A pollable parent end for pipe handle h: returns the socket (or
 * INVALID_SOCKET); the relay owns h from then on. */
static SOCKET start_relay(HANDLE h, bool to_child) {
    SOCKET sv[2];
    if (zell__w32_socketpair(sv) != 0) return INVALID_SOCKET;
    relay *r = malloc(sizeof *r);
    if (!r) { closesocket(sv[0]); closesocket(sv[1]); return INVALID_SOCKET; }
    r->pipe = h;
    r->sock = sv[0];
    r->to_child = to_child;
    if (!zell__plat_thread_start_detached(relay_main, r)) {
        free(r);
        closesocket(sv[0]);
        closesocket(sv[1]);
        return INVALID_SOCKET;
    }
    u_long one = 1;
    ioctlsocket(sv[1], FIONBIO, &one);
    return sv[1];
}

/* ---- overlapped named pipes (IOCP poller) ------------------------------------------------ */

#define PIPE_FD_BASE 0x400000
#define PIPE_SLOTS 4096
#define PIPE_CHUNK 65536

typedef struct pipe_slot {
    HANDLE h;          /* the parent's end; NULL = free slot */
    bool write;        /* the parent writes (the child's stdin) */
    bool wpending;     /* wov is in flight */
    OVERLAPPED wov;    /* the write in flight (its packet tells the poller) */
    HANDLE wev;
    char *wbuf;
} pipe_slot;

static pipe_slot g_pipes[PIPE_SLOTS];
static zell__plat_mutex g_pipes_mu = ZELL_PLAT_MUTEX_INIT;
static volatile LONG g_pipe_seq;

static pipe_slot *pipe_slot_of(int64_t fd) {
    if (fd < PIPE_FD_BASE || fd >= PIPE_FD_BASE + PIPE_SLOTS) return NULL;
    pipe_slot *s = &g_pipes[fd - PIPE_FD_BASE];
    return s->h ? s : NULL;
}

bool zell__w32_pipe_of(int64_t fd, HANDLE *h, bool *write) {
    pipe_slot *s = pipe_slot_of(fd);
    if (!s) return false;
    *h = s->h;
    *write = s->write;
    return true;
}

bool zell__w32_pipe_write_busy(int64_t fd) {
    pipe_slot *s = pipe_slot_of(fd);
    return s && s->wpending && !HasOverlappedIoCompleted(&s->wov);
}

bool zell__w32_pipe_write_ov(const void *ov, int64_t *fd) {
    const char *p = ov, *lo = (const char *)g_pipes, *hi = (const char *)(g_pipes + PIPE_SLOTS);
    if (p < lo || p >= hi) return false;
    size_t i = (size_t)(p - lo) / sizeof(pipe_slot);
    if ((const void *)&g_pipes[i].wov != ov) return false;
    *fd = PIPE_FD_BASE + (int64_t)i;
    return true;
}

/* A named pipe pair: *parent (overlapped, not inherited) and *child
 * (inheritable, synchronous). to_child: the parent writes. */
static int named_pipe(bool to_child, HANDLE *parent, HANDLE *child) {
    wchar_t name[96];
    _snwprintf(name, 95, L"\\\\.\\pipe\\zell-%lu-%ld-%lu", (unsigned long)GetCurrentProcessId(),
               (long)InterlockedIncrement(&g_pipe_seq), (unsigned long)GetTickCount());
    name[95] = 0;
    HANDLE p = CreateNamedPipeW(name, (to_child ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | 0x8 /* PIPE_REJECT_REMOTE_CLIENTS */, 1, PIPE_CHUNK,
                                PIPE_CHUNK, 0, NULL);
    if (p == INVALID_HANDLE_VALUE) return zell__w32_errno(GetLastError());
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE c = CreateFileW(name, to_child ? GENERIC_READ : GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
    if (c == INVALID_HANDLE_VALUE) {
        int e = zell__w32_errno(GetLastError());
        CloseHandle(p);
        return e;
    }
    *parent = p;
    *child = c;
    return 0;
}

/* Registers parent end h: its descriptor, or -1. */
static int64_t pipe_register(HANDLE h, bool write) {
    zell__plat_mutex_lock(&g_pipes_mu);
    int64_t fd = -1;
    for (int i = 0; i < PIPE_SLOTS; i++) {
        if (g_pipes[i].h) continue;
        pipe_slot *s = &g_pipes[i];
        memset(s, 0, sizeof *s);
        if (write) {
            s->wev = CreateEventW(NULL, TRUE, FALSE, NULL);
            s->wbuf = malloc(PIPE_CHUNK);
            if (!s->wev || !s->wbuf) {
                if (s->wev) CloseHandle(s->wev);
                free(s->wbuf);
                memset(s, 0, sizeof *s);
                break;
            }
        }
        s->write = write;
        s->h = h;
        fd = PIPE_FD_BASE + i;
        break;
    }
    zell__plat_mutex_unlock(&g_pipes_mu);
    return fd;
}

static _Thread_local HANDLE t_read_ev;

static int64_t named_read(pipe_slot *s, void *buf, int64_t n) {
    DWORD avail = 0;
    if (!PeekNamedPipe(s->h, NULL, 0, NULL, &avail, NULL)) {
        DWORD e = GetLastError();
        return e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED ? 0 : -EIO;
    }
    if (avail == 0) return -ZELL_PLAT_EAGAIN;
    DWORD want = n > (int64_t)avail ? avail : (DWORD)n;
    if (!t_read_ev) t_read_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!t_read_ev) return -EIO;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    /* low bit set: no packet on the poller's port for this read */
    ov.hEvent = (HANDLE)((uintptr_t)t_read_ev | 1);
    DWORD got = 0;
    if (!ReadFile(s->h, buf, want, &got, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            if (!GetOverlappedResult(s->h, &ov, &got, TRUE)) e = GetLastError();
            else return got;
        }
        return e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED ? 0 : -EIO;
    }
    return got;
}

static int64_t named_write(pipe_slot *s, const void *buf, int64_t n) {
    if (s->wpending) {
        if (!HasOverlappedIoCompleted(&s->wov)) return -ZELL_PLAT_EAGAIN;
        DWORD w = 0;
        BOOL ok = GetOverlappedResult(s->h, &s->wov, &w, FALSE);
        s->wpending = false;
        if (!ok) return -EPIPE;  /* the child closed its stdin */
    }
    if (n <= 0) return 0;
    DWORD k = n > PIPE_CHUNK ? PIPE_CHUNK : (DWORD)n;
    memcpy(s->wbuf, buf, k);
    memset(&s->wov, 0, sizeof s->wov);
    ResetEvent(s->wev);
    s->wov.hEvent = s->wev;
    if (WriteFile(s->h, s->wbuf, k, NULL, &s->wov)) {
        s->wpending = true;  /* done; collected by the next write */
        return k;
    }
    DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING) {
        s->wpending = true;
        return k;
    }
    return -EPIPE;  /* ERROR_NO_DATA / ERROR_BROKEN_PIPE: the reader is gone */
}

static void named_close(pipe_slot *s) {
    /* the last write still belongs to the child: let it drain (bounded) */
    if (s->wpending && !HasOverlappedIoCompleted(&s->wov) && WaitForSingleObject(s->wev, 10000) != WAIT_OBJECT_0)
        CancelIoEx(s->h, &s->wov);
    CloseHandle(s->h);
    if (s->wev) CloseHandle(s->wev);
    zell__plat_mutex_lock(&g_pipes_mu);
    free(s->wbuf);
    memset(s, 0, sizeof *s);
    zell__plat_mutex_unlock(&g_pipes_mu);
}

/* ---- spawn --------------------------------------------------------------------------- */

static HANDLE inheritable(HANDLE h) {
    HANDLE d = NULL;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS)) return NULL;
    return d;
}

int zell__plat_spawn(char *const *argv, char *const *envp, const char *dir, const int modes[3],
                      zell__plat_child *out, const char **op) {
    zell__w32_wsa_init();
    *op = "exec";
    wchar_t *cmd = command_line(argv);
    wchar_t *wenv = envp ? env_block(envp) : NULL;
    wchar_t *wdir = dir ? zell__w32_wpath(dir) : NULL;
    if (!cmd || (envp && !wenv) || (dir && !wdir)) {
        free(cmd);
        free(wenv);
        free(wdir);
        return EINVAL;
    }
    static const DWORD std_ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE child_h[3] = {NULL, NULL, NULL}, parent_h[3] = {NULL, NULL, NULL};
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    int err = 0;
    bool named = zell__w32_poller_waits_pipes();
    zell__plat_mutex_lock(&g_spawn_mu);
    for (int i = 0; i < 3 && !err; i++) {
        if (modes[i] == ZELL_PLAT_STDIO_PIPE && named) {
            err = named_pipe(i == 0, &parent_h[i], &child_h[i]);
            if (err) *op = "pipe";
        } else if (modes[i] == ZELL_PLAT_STDIO_PIPE) {
            HANDLE rd, wr;
            if (!CreatePipe(&rd, &wr, &sa, 0)) { err = zell__w32_errno(GetLastError()); *op = "pipe"; break; }
            child_h[i] = i == 0 ? rd : wr;
            parent_h[i] = i == 0 ? wr : rd;
            SetHandleInformation(parent_h[i], HANDLE_FLAG_INHERIT, 0);
        } else if (modes[i] == ZELL_PLAT_STDIO_NULL) {
            child_h[i] = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                     OPEN_EXISTING, 0, NULL);
            if (child_h[i] == INVALID_HANDLE_VALUE) child_h[i] = NULL;
        } else {
            child_h[i] = inheritable(GetStdHandle(std_ids[i]));
        }
    }
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (!err) {
        STARTUPINFOW si;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = child_h[0];
        si.hStdOutput = child_h[1];
        si.hStdError = child_h[2];
        if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, wenv, wdir, &si, &pi))
            err = zell__w32_errno(GetLastError());
    }
    for (int i = 0; i < 3; i++)
        if (child_h[i]) CloseHandle(child_h[i]);
    zell__plat_mutex_unlock(&g_spawn_mu);
    free(cmd);
    free(wenv);
    free(wdir);
    if (err) {
        for (int i = 0; i < 3; i++)
            if (parent_h[i]) CloseHandle(parent_h[i]);
        return err;
    }
    CloseHandle(pi.hThread);
    out->pid = pi.dwProcessId;
    out->pidfd = -1;  /* exit is polled (wait_pid) */
    for (int i = 0; i < 3; i++) {
        out->fds[i] = -1;
        if (!parent_h[i]) continue;
        if (named) {
            out->fds[i] = pipe_register(parent_h[i], i == 0);
            if (out->fds[i] < 0) CloseHandle(parent_h[i]);
            continue;
        }
        SOCKET s = start_relay(parent_h[i], i == 0);
        if (s == INVALID_SOCKET) {
            CloseHandle(parent_h[i]);
            continue;
        }
        out->fds[i] = (int64_t)s;
    }
    child_rec *c = calloc(1, sizeof *c);
    if (c) {
        c->pid = out->pid;
        c->h = pi.hProcess;
        zell__plat_mutex_lock(&g_mu);
        c->next = g_children;
        g_children = c;
        zell__plat_mutex_unlock(&g_mu);
    } else {
        CloseHandle(pi.hProcess);
    }
    *op = NULL;
    return 0;
}

/* ---- pipe ends (named pipes, or relay sockets) ------------------------------------------ */

static int64_t sock_result(int r) {
    if (r >= 0) return r;
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) return -ZELL_PLAT_EAGAIN;
    if (e == WSAECONNRESET || e == WSAECONNABORTED || e == WSAESHUTDOWN) return -EPIPE;
    return -EIO;
}

int64_t zell__plat_pipe_read(int64_t fd, void *buf, int64_t n) {
    pipe_slot *ps = pipe_slot_of(fd);
    if (ps) return named_read(ps, buf, n);
    int64_t r = sock_result(recv((SOCKET)fd, buf, n > 0x40000000 ? 0x40000000 : (int)n, 0));
    return r == -EPIPE ? 0 : r;  /* the relay went away: EOF */
}

int64_t zell__plat_pipe_write(int64_t fd, const void *buf, int64_t n) {
    pipe_slot *ps = pipe_slot_of(fd);
    if (ps) return named_write(ps, buf, n);
    return sock_result(send((SOCKET)fd, buf, n > 0x40000000 ? 0x40000000 : (int)n, 0));
}

void zell__plat_pipe_close(int64_t fd) {
    pipe_slot *ps = pipe_slot_of(fd);
    if (ps) {
        named_close(ps);
        return;
    }
    if (fd >= 0) closesocket((SOCKET)fd);
}

/* ---- wait and kill ---------------------------------------------------------------------- */

int zell__plat_wait_pid(int64_t pid, int64_t *status) {
    zell__plat_mutex_lock(&g_mu);
    child_rec *c = find(pid);
    if (!c) {
        zell__plat_mutex_unlock(&g_mu);
        return -ECHILD;
    }
    HANDLE h = c->h;
    zell__plat_mutex_unlock(&g_mu);
    DWORD w = WaitForSingleObject(h, 0);
    if (w == WAIT_TIMEOUT) return 0;
    if (w != WAIT_OBJECT_0) return -zell__w32_errno(GetLastError());
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    zell__plat_mutex_lock(&g_mu);
    child_rec **pp = &g_children;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    int killed = c->killed;
    zell__plat_mutex_unlock(&g_mu);
    CloseHandle(h);
    free(c);
    *status = killed ? 1000 + killed : (int64_t)code;
    return 1;
}

int zell__plat_kill(int64_t pid, int sig) {
    zell__plat_mutex_lock(&g_mu);
    child_rec *c = find(pid);
    HANDLE h = c ? c->h : NULL;
    bool own = false;
    if (!h) {
        h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
        own = h != NULL;
    }
    int e = 0;
    if (!h) {
        DWORD le = GetLastError();
        e = le == ERROR_INVALID_PARAMETER ? 0 : zell__w32_errno(le);  /* gone */
    } else if (sig != 0) {
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
            if (TerminateProcess(h, 1)) {
                if (c) c->killed = sig;
            } else {
                e = zell__w32_errno(GetLastError());
            }
        }
    }
    zell__plat_mutex_unlock(&g_mu);
    if (own) CloseHandle(h);
    return e;
}

#else
typedef int zell__proc_win32_unused;
#endif
