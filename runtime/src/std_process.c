/* std_process.c - child processes for std.process (stdlib_src/process.sl).
 *
 * rt_spawn forks and execs with optional pipes for stdin/stdout/stderr;
 * the parent's pipe ends are non-blocking and waited on through the
 * scheduler's readiness waits (slang_io_wait_readable/writable, ABI.md
 * 10.9), so reading a child's output never blocks a worker. Exit is
 * observed through a pidfd (Linux >= 5.3) when available; otherwise the
 * Slang side polls rt_wait_pid. POSIX only (like std_fs.c). */
#include "rt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

void slang_process_rt_spawn(const slang_string *path, const slang_slice *args, const slang_slice *env, bool inherit_env,
                            const slang_string *dir, int64_t in_mode, int64_t out_mode, int64_t err_mode,
                            slang_slice *out, slang_error **err);
int64_t slang_process_rt_read(int64_t fd, const slang_slice *buf, slang_error **err);
int64_t slang_process_rt_write(int64_t fd, const slang_slice *buf, int64_t off, slang_error **err);
void slang_process_rt_close(int64_t fd);
int64_t slang_process_rt_wait_pid(int64_t pid, slang_error **err);
void slang_process_rt_kill(int64_t pid, int64_t sig, slang_error **err);
bool slang_process_rt_executable(const slang_string *path);
slang_task *slang_process_rt_wait_readable(int64_t fd, int64_t ms);
slang_task *slang_process_rt_wait_writable(int64_t fd, int64_t ms);

static const char *sdata(const slang_string *s) { return s ? s->data : ""; }

static char *dupstr(const slang_string *s) {
    int64_t n = s ? s->len : 0;
    char *p = malloc((size_t)n + 1);
    if (!p) slang_panic("out of memory (process)");
    if (n) memcpy(p, s->data, (size_t)n);
    p[n] = 0;
    return p;
}

/* NULL-terminated copy of a []string. */
static char **argv_of(const slang_string *first, const slang_slice *xs) {
    int64_t n = xs ? xs->len : 0, k = 0;
    char **v = calloc((size_t)n + 2, sizeof *v);
    if (!v) slang_panic("out of memory (process)");
    if (first) v[k++] = dupstr(first);
    for (int64_t i = 0; i < n; i++) v[k++] = dupstr(((slang_string **)xs->data)[i]);
    v[k] = NULL;
    return v;
}

static void free_argv(char **v) {
    for (char **p = v; *p; p++) free(*p);
    free(v);
}

static void nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

/* Modes: 0 inherit, 1 pipe, 2 /dev/null. out = [pid, pidfd, stdin write
 * end, stdout read end, stderr read end] (-1 = none). */
void slang_process_rt_spawn(const slang_string *path, const slang_slice *args, const slang_slice *env, bool inherit_env,
                            const slang_string *dir, int64_t in_mode, int64_t out_mode, int64_t err_mode,
                            slang_slice *out, slang_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    int pin[2] = {-1, -1}, pout[2] = {-1, -1}, perr[2] = {-1, -1};
    int ex[2];
    if ((in_mode == 1 && pipe(pin) != 0) || (out_mode == 1 && pipe(pout) != 0) || (err_mode == 1 && pipe(perr) != 0) ||
        pipe(ex) != 0) {
        if (err) *err = slang_error_errno("pipe", "process", errno);
        return;
    }
    fcntl(ex[1], F_SETFD, FD_CLOEXEC);
    /* no pipe end may leak into the child beyond its 0/1/2 (dup2 clears
     * the flag on those), or the child would keep its own stdin open */
    int ends[6] = {pin[0], pin[1], pout[0], pout[1], perr[0], perr[1]};
    for (int i = 0; i < 6; i++)
        if (ends[i] >= 0) fcntl(ends[i], F_SETFD, FD_CLOEXEC);
    char **argv = argv_of(path, args);
    char **envp = NULL;
    if (!inherit_env || (env && env->len > 0)) {
        /* explicit environment: inherited entries (if asked) overridden by env */
        int64_t base = 0;
        if (inherit_env)
            for (char **e = environ; *e; e++) base++;
        int64_t n = env ? env->len : 0;
        envp = calloc((size_t)(base + n + 1), sizeof *envp);
        int64_t k = 0;
        for (int64_t i = 0; i < n; i++) envp[k++] = dupstr(((slang_string **)env->data)[i]);
        if (inherit_env) {
            for (char **e = environ; *e; e++) {
                const char *eq = strchr(*e, '=');
                size_t kl = eq ? (size_t)(eq - *e) : strlen(*e);
                bool dup = false;
                for (int64_t i = 0; i < n && !dup; i++) dup = strncmp(envp[i], *e, kl) == 0 && envp[i][kl] == '=';
                if (!dup) envp[k++] = strdup(*e);
            }
        }
        envp[k] = NULL;
    }
    char *cdir = dir && dir->len ? dupstr(dir) : NULL;
    pid_t pid = fork();
    if (pid == 0) {
        /* child: only async-signal-safe calls until exec */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        int devnull = -1;
        if (in_mode == 2 || out_mode == 2 || err_mode == 2) devnull = open("/dev/null", O_RDWR);
        if (in_mode == 1) dup2(pin[0], 0); else if (in_mode == 2) dup2(devnull, 0);
        if (out_mode == 1) dup2(pout[1], 1); else if (out_mode == 2) dup2(devnull, 1);
        if (err_mode == 1) dup2(perr[1], 2); else if (err_mode == 2) dup2(devnull, 2);
        if (cdir && chdir(cdir) != 0) {
            int e = errno;
            ssize_t w = write(ex[1], &e, sizeof e);
            (void)w;
            _exit(127);
        }
        if (envp) execvpe(argv[0], argv, envp); else execvp(argv[0], argv);
        int e = errno;
        ssize_t w = write(ex[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    int fork_errno = errno;
    close(ex[1]);
    if (pin[0] >= 0) close(pin[0]);
    if (pout[1] >= 0) close(pout[1]);
    if (perr[1] >= 0) close(perr[1]);
    free_argv(argv);
    if (envp) free_argv(envp);
    free(cdir);
    if (pid < 0) {
        close(ex[0]);
        if (pin[1] >= 0) close(pin[1]);
        if (pout[0] >= 0) close(pout[0]);
        if (perr[0] >= 0) close(perr[0]);
        if (err) *err = slang_error_errno("fork", "process", fork_errno);
        return;
    }
    int child_errno = 0;
    ssize_t r;
    do r = read(ex[0], &child_errno, sizeof child_errno);
    while (r < 0 && errno == EINTR);
    close(ex[0]);
    if (r == (ssize_t)sizeof child_errno) {
        int st;
        waitpid(pid, &st, 0);
        if (pin[1] >= 0) close(pin[1]);
        if (pout[0] >= 0) close(pout[0]);
        if (perr[0] >= 0) close(perr[0]);
        if (err) *err = slang_error_errno("exec", sdata(path), child_errno);
        return;
    }
    int pidfd = -1;
#ifdef SYS_pidfd_open
    pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd >= 0) fcntl(pidfd, F_SETFD, FD_CLOEXEC);
#endif
    if (pin[1] >= 0) nonblock(pin[1]);
    if (pout[0] >= 0) nonblock(pout[0]);
    if (perr[0] >= 0) nonblock(perr[0]);
    slang_slice_new(out, 8, 5, 5);
    int64_t *o = out->data;
    o[0] = pid;
    o[1] = pidfd;
    o[2] = pin[1];
    o[3] = pout[0];
    o[4] = perr[0];
}

/* Non-blocking read: bytes read, 0 at EOF, -1 if it would block. */
int64_t slang_process_rt_read(int64_t fd, const slang_slice *buf, slang_error **err) {
    if (err) *err = NULL;
    if (!buf || buf->len == 0) return 0;
    for (;;) {
        ssize_t n = read((int)fd, buf->data, (size_t)buf->len);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        if (err) *err = slang_error_errno("read", "pipe", errno);
        return 0;
    }
}

/* Non-blocking write of buf[off..]: bytes written (maybe 0), -1 if it
 * would block. */
int64_t slang_process_rt_write(int64_t fd, const slang_slice *buf, int64_t off, slang_error **err) {
    if (err) *err = NULL;
    int64_t len = buf ? buf->len : 0;
    if (off >= len) return 0;
    for (;;) {
        ssize_t n = write((int)fd, (const char *)buf->data + off, (size_t)(len - off));
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        if (err) *err = slang_error_errno("write", "pipe", errno);
        return 0;
    }
}

void slang_process_rt_close(int64_t fd) {
    if (fd < 0) return;
    slang_io_forget(fd);
    close((int)fd);
}

/* -1 while running; else the exit code, or 1000 + signal number. */
int64_t slang_process_rt_wait_pid(int64_t pid, slang_error **err) {
    if (err) *err = NULL;
    int st;
    pid_t r;
    do r = waitpid((pid_t)pid, &st, WNOHANG);
    while (r < 0 && errno == EINTR);
    if (r == 0) return -1;
    if (r < 0) {
        if (err) *err = slang_error_errno("wait", "process", errno);
        return 0;
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 1000 + WTERMSIG(st);
    return -1;
}

void slang_process_rt_kill(int64_t pid, int64_t sig, slang_error **err) {
    if (err) *err = NULL;
    if (kill((pid_t)pid, (int)sig) != 0 && errno != ESRCH) {
        if (err) *err = slang_error_errno("kill", "process", errno);
    }
}

bool slang_process_rt_executable(const slang_string *path) {
    struct stat st;
    const char *p = sdata(path);
    return *p && stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, X_OK) == 0;
}

slang_task *slang_process_rt_wait_readable(int64_t fd, int64_t ms) { return slang_io_wait_readable(fd, ms); }
slang_task *slang_process_rt_wait_writable(int64_t fd, int64_t ms) { return slang_io_wait_writable(fd, ms); }
