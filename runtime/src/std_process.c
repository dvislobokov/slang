/* std_process.c - child processes for std.process (stdlib_src/process.zl).
 *
 * rt_spawn forks and execs with optional pipes for stdin/stdout/stderr;
 * the parent's pipe ends are non-blocking and waited on through the
 * scheduler's readiness waits (zell_io_wait_readable/writable, ABI.md
 * 10.9), so reading a child's output never blocks a worker. Exit is
 * observed through a pidfd (Linux >= 5.3) when available; otherwise the
 * Zell side polls rt_wait_pid. The system calls are in the platform layer
 * (zell__plat_spawn & co.: fork/exec on POSIX, CreateProcessW on Windows). */
#include "rt_internal.h"

#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zell_process_rt_spawn(const zell_string *path, const zell_slice *args, const zell_slice *env, bool inherit_env,
                            const zell_string *dir, int64_t in_mode, int64_t out_mode, int64_t err_mode,
                            zell_slice *out, zell_error **err);
int64_t zell_process_rt_read(int64_t fd, const zell_slice *buf, zell_error **err);
int64_t zell_process_rt_write(int64_t fd, const zell_slice *buf, int64_t off, zell_error **err);
void zell_process_rt_close(int64_t fd);
int64_t zell_process_rt_wait_pid(int64_t pid, zell_error **err);
void zell_process_rt_kill(int64_t pid, int64_t sig, zell_error **err);
bool zell_process_rt_executable(const zell_string *path);
zell_task *zell_process_rt_wait_readable(int64_t fd, int64_t ms);
zell_task *zell_process_rt_wait_writable(int64_t fd, int64_t ms);

/* NUL-terminated: these bytes go to C APIs (views are copied, ABI.md 4.1). */
static const char *sdata(const zell_string *s) { return zell_string_cstr(s); }

static char *dupstr(const zell_string *s) {
    int64_t n = s ? s->len : 0;
    char *p = malloc((size_t)n + 1);
    if (!p) zell_panic("out of memory (process)");
    if (n) memcpy(p, s->data, (size_t)n);
    p[n] = 0;
    return p;
}

/* NULL-terminated copy of a []string. */
static char **argv_of(const zell_string *first, const zell_slice *xs) {
    int64_t n = xs ? xs->len : 0, k = 0;
    char **v = calloc((size_t)n + 2, sizeof *v);
    if (!v) zell_panic("out of memory (process)");
    if (first) v[k++] = dupstr(first);
    for (int64_t i = 0; i < n; i++) v[k++] = dupstr(((zell_string **)xs->data)[i]);
    v[k] = NULL;
    return v;
}

static void free_argv(char **v) {
    for (char **p = v; *p; p++) free(*p);
    free(v);
}

/* Modes: 0 inherit, 1 pipe, 2 /dev/null. out = [pid, pidfd, stdin write
 * end, stdout read end, stderr read end] (-1 = none). */
void zell_process_rt_spawn(const zell_string *path, const zell_slice *args, const zell_slice *env, bool inherit_env,
                            const zell_string *dir, int64_t in_mode, int64_t out_mode, int64_t err_mode,
                            zell_slice *out, zell_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    char **argv = argv_of(path, args);
    char **envp = NULL;
    if (!inherit_env || (env && env->len > 0)) {
        /* explicit environment: inherited entries (if asked) overridden by env */
        char **base = inherit_env ? zell__plat_environ() : NULL;
        int64_t nbase = 0;
        if (base)
            for (char **e = base; *e; e++) nbase++;
        int64_t n = env ? env->len : 0;
        envp = calloc((size_t)(nbase + n + 1), sizeof *envp);
        if (!envp) zell_panic("out of memory (process)");
        int64_t k = 0;
        for (int64_t i = 0; i < n; i++) envp[k++] = dupstr(((zell_string **)env->data)[i]);
        for (int64_t j = 0; j < nbase; j++) {
            const char *e = base[j];
            const char *eq = strchr(e, '=');
            size_t kl = eq ? (size_t)(eq - e) : strlen(e);
            bool dup = false;
            for (int64_t i = 0; i < n && !dup; i++) dup = strncmp(envp[i], e, kl) == 0 && envp[i][kl] == '=';
            if (!dup) envp[k++] = strdup(e);
        }
        envp[k] = NULL;
        zell__plat_free_strv(base);
    }
    char *cdir = dir && dir->len ? dupstr(dir) : NULL;
    int modes[3] = {(int)in_mode, (int)out_mode, (int)err_mode};
    zell__plat_child c;
    const char *op = NULL;
    int e = zell__plat_spawn(argv, envp, cdir, modes, &c, &op);
    free_argv(argv);
    if (envp) free_argv(envp);
    free(cdir);
    if (e) {
        if (err) *err = zell_error_errno(op ? op : "exec", op && strcmp(op, "exec") == 0 ? sdata(path) : "process", e);
        return;
    }
    zell_slice_new(out, 8, 5, 5);
    int64_t *o = out->data;
    o[0] = c.pid;
    o[1] = c.pidfd;
    o[2] = c.fds[0];
    o[3] = c.fds[1];
    o[4] = c.fds[2];
}

/* Non-blocking read: bytes read, 0 at EOF, -1 if it would block. */
int64_t zell_process_rt_read(int64_t fd, const zell_slice *buf, zell_error **err) {
    if (err) *err = NULL;
    if (!buf || buf->len == 0) return 0;
    int64_t n = zell__plat_pipe_read(fd, buf->data, buf->len);
    if (n >= 0) return n;
    if (n == -ZELL_PLAT_EAGAIN) return -1;
    if (err) *err = zell_error_errno("read", "pipe", (int)-n);
    return 0;
}

/* Non-blocking write of buf[off..]: bytes written (maybe 0), -1 if it
 * would block. */
int64_t zell_process_rt_write(int64_t fd, const zell_slice *buf, int64_t off, zell_error **err) {
    if (err) *err = NULL;
    int64_t len = buf ? buf->len : 0;
    if (off >= len) return 0;
    int64_t n = zell__plat_pipe_write(fd, (const char *)buf->data + off, len - off);
    if (n >= 0) return n;
    if (n == -ZELL_PLAT_EAGAIN) return -1;
    if (err) *err = zell_error_errno("write", "pipe", (int)-n);
    return 0;
}

void zell_process_rt_close(int64_t fd) {
    if (fd < 0) return;
    zell_io_forget(fd);
    zell__plat_pipe_close(fd);
}

/* -1 while running; else the exit code, or 1000 + signal number. */
int64_t zell_process_rt_wait_pid(int64_t pid, zell_error **err) {
    if (err) *err = NULL;
    int64_t st = 0;
    int r = zell__plat_wait_pid(pid, &st);
    if (r == 0) return -1;
    if (r < 0) {
        if (err) *err = zell_error_errno("wait", "process", -r);
        return 0;
    }
    return st;
}

void zell_process_rt_kill(int64_t pid, int64_t sig, zell_error **err) {
    if (err) *err = NULL;
    int e = zell__plat_kill(pid, (int)sig);
    if (e && err) *err = zell_error_errno("kill", "process", e);
}

bool zell_process_rt_executable(const zell_string *path) { return zell__plat_is_executable(sdata(path)); }

zell_task *zell_process_rt_wait_readable(int64_t fd, int64_t ms) { return zell_io_wait_readable(fd, ms); }
zell_task *zell_process_rt_wait_writable(int64_t fd, int64_t ms) { return zell_io_wait_writable(fd, ms); }
