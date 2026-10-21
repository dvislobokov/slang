/* proc_win32.c - Windows child processes for the platform layer
 * (platform.h): CreateProcessW with a quoted UTF-16 command line and
 * environment block.
 *
 * Anonymous pipes cannot be waited on by the readiness poller (WSAPoll
 * takes sockets only), so a piped stream is relayed: the child gets the
 * pipe end, a relay thread copies between the parent's pipe end and one
 * end of a loopback socket pair, and the parent works with the other
 * socket end, which is non-blocking and pollable like any socket. EOF
 * propagates both ways (the relay shuts its socket down when the child
 * closes its end, and closes the pipe when the parent closes its socket).
 * Process handles are kept in a table by pid until reaped. */
#include "platform.h"

#if defined(_WIN32)

#include "win32_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- handles of live children ----------------------------------------------- */

typedef struct child_rec { int64_t pid; HANDLE h; int killed; struct child_rec *next; } child_rec;
static slang__plat_mutex g_mu = SLANG_PLAT_MUTEX_INIT;
static slang__plat_mutex g_spawn_mu = SLANG_PLAT_MUTEX_INIT;  /* one spawn at a time: inheritable handles */
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
    wchar_t *w = slang__w32_wide(buf ? buf : "");
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
    if (slang__w32_socketpair(sv) != 0) return INVALID_SOCKET;
    relay *r = malloc(sizeof *r);
    if (!r) { closesocket(sv[0]); closesocket(sv[1]); return INVALID_SOCKET; }
    r->pipe = h;
    r->sock = sv[0];
    r->to_child = to_child;
    if (!slang__plat_thread_start_detached(relay_main, r)) {
        free(r);
        closesocket(sv[0]);
        closesocket(sv[1]);
        return INVALID_SOCKET;
    }
    u_long one = 1;
    ioctlsocket(sv[1], FIONBIO, &one);
    return sv[1];
}

/* ---- spawn --------------------------------------------------------------------------- */

static HANDLE inheritable(HANDLE h) {
    HANDLE d = NULL;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS)) return NULL;
    return d;
}

int slang__plat_spawn(char *const *argv, char *const *envp, const char *dir, const int modes[3],
                      slang__plat_child *out, const char **op) {
    slang__w32_wsa_init();
    *op = "exec";
    wchar_t *cmd = command_line(argv);
    wchar_t *wenv = envp ? env_block(envp) : NULL;
    wchar_t *wdir = dir ? slang__w32_wpath(dir) : NULL;
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
    slang__plat_mutex_lock(&g_spawn_mu);
    for (int i = 0; i < 3 && !err; i++) {
        if (modes[i] == SLANG_PLAT_STDIO_PIPE) {
            HANDLE rd, wr;
            if (!CreatePipe(&rd, &wr, &sa, 0)) { err = slang__w32_errno(GetLastError()); *op = "pipe"; break; }
            child_h[i] = i == 0 ? rd : wr;
            parent_h[i] = i == 0 ? wr : rd;
            SetHandleInformation(parent_h[i], HANDLE_FLAG_INHERIT, 0);
        } else if (modes[i] == SLANG_PLAT_STDIO_NULL) {
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
            err = slang__w32_errno(GetLastError());
    }
    for (int i = 0; i < 3; i++)
        if (child_h[i]) CloseHandle(child_h[i]);
    slang__plat_mutex_unlock(&g_spawn_mu);
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
        slang__plat_mutex_lock(&g_mu);
        c->next = g_children;
        g_children = c;
        slang__plat_mutex_unlock(&g_mu);
    } else {
        CloseHandle(pi.hProcess);
    }
    *op = NULL;
    return 0;
}

/* ---- pipe ends (sockets) --------------------------------------------------------------- */

static int64_t sock_result(int r) {
    if (r >= 0) return r;
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) return -SLANG_PLAT_EAGAIN;
    if (e == WSAECONNRESET || e == WSAECONNABORTED || e == WSAESHUTDOWN) return -EPIPE;
    return -EIO;
}

int64_t slang__plat_pipe_read(int64_t fd, void *buf, int64_t n) {
    int64_t r = sock_result(recv((SOCKET)fd, buf, n > 0x40000000 ? 0x40000000 : (int)n, 0));
    return r == -EPIPE ? 0 : r;  /* the relay went away: EOF */
}

int64_t slang__plat_pipe_write(int64_t fd, const void *buf, int64_t n) {
    return sock_result(send((SOCKET)fd, buf, n > 0x40000000 ? 0x40000000 : (int)n, 0));
}

void slang__plat_pipe_close(int64_t fd) {
    if (fd >= 0) closesocket((SOCKET)fd);
}

/* ---- wait and kill ---------------------------------------------------------------------- */

int slang__plat_wait_pid(int64_t pid, int64_t *status) {
    slang__plat_mutex_lock(&g_mu);
    child_rec *c = find(pid);
    if (!c) {
        slang__plat_mutex_unlock(&g_mu);
        return -ECHILD;
    }
    HANDLE h = c->h;
    slang__plat_mutex_unlock(&g_mu);
    DWORD w = WaitForSingleObject(h, 0);
    if (w == WAIT_TIMEOUT) return 0;
    if (w != WAIT_OBJECT_0) return -slang__w32_errno(GetLastError());
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    slang__plat_mutex_lock(&g_mu);
    child_rec **pp = &g_children;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    int killed = c->killed;
    slang__plat_mutex_unlock(&g_mu);
    CloseHandle(h);
    free(c);
    *status = killed ? 1000 + killed : (int64_t)code;
    return 1;
}

int slang__plat_kill(int64_t pid, int sig) {
    slang__plat_mutex_lock(&g_mu);
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
        e = le == ERROR_INVALID_PARAMETER ? 0 : slang__w32_errno(le);  /* gone */
    } else if (sig != 0) {
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
            if (TerminateProcess(h, 1)) {
                if (c) c->killed = sig;
            } else {
                e = slang__w32_errno(GetLastError());
            }
        }
    }
    slang__plat_mutex_unlock(&g_mu);
    if (own) CloseHandle(h);
    return e;
}

#else
typedef int slang__proc_win32_unused;
#endif
