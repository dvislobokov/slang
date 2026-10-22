/* fs_posix.c - POSIX files and directories, child processes, signals and
 * the user database for the platform layer (platform.h). The std_*.c
 * modules marshal Zell values; the system calls are here. */
#include "platform.h"

#if defined(__unix__) || defined(__APPLE__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

/* ---- files ---------------------------------------------------------------- */

static int open_flags(int f) {
    int o = (f & 3) == ZELL_PLAT_O_WRONLY ? O_WRONLY : (f & 3) == ZELL_PLAT_O_RDWR ? O_RDWR : O_RDONLY;
    if (f & ZELL_PLAT_O_APPEND) o |= O_APPEND;
    if (f & ZELL_PLAT_O_CREATE) o |= O_CREAT;
    if (f & ZELL_PLAT_O_EXCL) o |= O_EXCL;
    if (f & ZELL_PLAT_O_SYNC) o |= O_SYNC;
    if (f & ZELL_PLAT_O_TRUNC) o |= O_TRUNC;
    return o | O_CLOEXEC;
}

int64_t zell__plat_open(const char *path, int flags, int perm) {
    int fd;
    do fd = open(path, open_flags(flags), (mode_t)perm);
    while (fd < 0 && errno == EINTR);
    return fd < 0 ? -(int64_t)errno : fd;
}

int64_t zell__plat_read(int64_t fd, void *buf, int64_t n) {
    ssize_t r;
    do r = read((int)fd, buf, (size_t)n);
    while (r < 0 && errno == EINTR);
    return r < 0 ? -(int64_t)errno : r;
}

int64_t zell__plat_pread(int64_t fd, void *buf, int64_t n, int64_t off) {
    ssize_t r;
    do r = pread((int)fd, buf, (size_t)n, (off_t)off);
    while (r < 0 && errno == EINTR);
    return r < 0 ? -(int64_t)errno : r;
}

int64_t zell__plat_write(int64_t fd, const void *buf, int64_t n) {
    ssize_t r;
    do r = write((int)fd, buf, (size_t)n);
    while (r < 0 && errno == EINTR);
    return r < 0 ? -(int64_t)errno : r;
}

int zell__plat_write_all(int64_t fd, const void *buf, int64_t n) {
    const char *p = buf;
    while (n > 0) {
        int64_t w = zell__plat_write(fd, p, n);
        if (w < 0) return (int)-w;
        p += w;
        n -= w;
    }
    return 0;
}

int64_t zell__plat_seek(int64_t fd, int64_t off, int whence) {
    int w = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
    off_t r = lseek((int)fd, (off_t)off, w);
    return r < 0 ? -(int64_t)errno : (int64_t)r;
}

int zell__plat_close(int64_t fd) {
    /* close(2) releases the descriptor even on EINTR: never retry. */
    return close((int)fd) != 0 && errno != EINTR ? errno : 0;
}

int zell__plat_fsync(int64_t fd) { return fsync((int)fd) == 0 ? 0 : errno; }
int zell__plat_ftruncate(int64_t fd, int64_t size) { return ftruncate((int)fd, (off_t)size) == 0 ? 0 : errno; }

static void stat_of(const struct stat *st, zell__plat_stat *o) {
    o->size = (int64_t)st->st_size;
    o->perm = (uint32_t)(st->st_mode & 07777);
    o->kind = S_ISREG(st->st_mode) ? ZELL_PLAT_KIND_FILE : S_ISDIR(st->st_mode) ? ZELL_PLAT_KIND_DIR
            : S_ISLNK(st->st_mode) ? ZELL_PLAT_KIND_LINK : ZELL_PLAT_KIND_OTHER;
#if defined(__APPLE__)
    o->mtime_sec = (int64_t)st->st_mtimespec.tv_sec;
    o->mtime_nsec = (int32_t)st->st_mtimespec.tv_nsec;
#else
    o->mtime_sec = (int64_t)st->st_mtim.tv_sec;
    o->mtime_nsec = (int32_t)st->st_mtim.tv_nsec;
#endif
}

int zell__plat_fstat(int64_t fd, zell__plat_stat *o) {
    struct stat st;
    if (fstat((int)fd, &st) != 0) return errno;
    stat_of(&st, o);
    return 0;
}

int zell__plat_stat_path(const char *path, bool follow, zell__plat_stat *o) {
    struct stat st;
    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) return errno;
    stat_of(&st, o);
    return 0;
}

bool zell__plat_isatty(int64_t fd) { return isatty((int)fd) == 1; }
int zell__plat_mkdir(const char *path) { return mkdir(path, 0777) == 0 ? 0 : errno; }
int zell__plat_rmdir(const char *path) { return rmdir(path) == 0 ? 0 : errno; }
int zell__plat_unlink_file(const char *path) { return unlink(path) == 0 ? 0 : errno; }
int zell__plat_remove(const char *path) { return remove(path) == 0 ? 0 : errno; }
int zell__plat_rename(const char *from, const char *to) { return rename(from, to) == 0 ? 0 : errno; }
int zell__plat_chmod(const char *path, int mode) { return chmod(path, (mode_t)mode) == 0 ? 0 : errno; }
int zell__plat_chdir(const char *path) { return chdir(path) == 0 ? 0 : errno; }

char *zell__plat_getcwd(int *err) {
    char *p = getcwd(NULL, 0);
    *err = p ? 0 : errno;
    return p;
}

bool zell__plat_is_executable(const char *path) {
    struct stat st;
    return *path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

struct zell__plat_dir { DIR *d; };

zell__plat_dir *zell__plat_dir_open(const char *path, int *err) {
    DIR *d = opendir(path);
    if (!d) { *err = errno; return NULL; }
    zell__plat_dir *r = malloc(sizeof *r);
    if (!r) { closedir(d); *err = ENOMEM; return NULL; }
    r->d = d;
    *err = 0;
    return r;
}

const char *zell__plat_dir_next(zell__plat_dir *d, int *kind, int *err) {
    *err = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d->d);
        if (!e) { *err = errno; return NULL; }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        switch (e->d_type) {
        case DT_REG: *kind = ZELL_PLAT_KIND_FILE; break;
        case DT_DIR: *kind = ZELL_PLAT_KIND_DIR; break;
        case DT_LNK: *kind = ZELL_PLAT_KIND_LINK; break;
        case DT_UNKNOWN: *kind = -1; break;
        default: *kind = ZELL_PLAT_KIND_OTHER; break;
        }
        return e->d_name;
    }
}

void zell__plat_dir_close(zell__plat_dir *d) {
    if (!d) return;
    closedir(d->d);
    free(d);
}

/* ---- child processes ------------------------------------------------------ */

static void nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

#if defined(__APPLE__) || defined(__OpenBSD__)
/* execvpe is a GNU/FreeBSD extension: search PATH by hand. */
static void exec_env(char *const *argv, char *const *envp) {
    const char *f = argv[0];
    if (strchr(f, '/')) { execve(f, argv, envp); return; }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    char buf[4096];
    while (*path) {
        const char *c = strchr(path, ':');
        size_t n = c ? (size_t)(c - path) : strlen(path);
        if (n + strlen(f) + 2 < sizeof buf) {
            memcpy(buf, n ? path : ".", n ? n : 1);
            size_t k = n ? n : 1;
            buf[k] = '/';
            strcpy(buf + k + 1, f);
            execve(buf, argv, envp);
        }
        path += n + (c ? 1 : 0);
    }
}
#else
static void exec_env(char *const *argv, char *const *envp) { execvpe(argv[0], argv, envp); }
#endif

int zell__plat_spawn(char *const *argv, char *const *envp, const char *dir, const int modes[3],
                      zell__plat_child *out, const char **op) {
    int p[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
    int ex[2];
    *op = "pipe";
    for (int i = 0; i < 3; i++)
        if (modes[i] == ZELL_PLAT_STDIO_PIPE && pipe(p[i]) != 0) {
            int e = errno;
            for (int j = 0; j < i; j++) { close(p[j][0]); close(p[j][1]); }
            return e;
        }
    if (pipe(ex) != 0) {
        int e = errno;
        for (int j = 0; j < 3; j++) if (p[j][0] >= 0) { close(p[j][0]); close(p[j][1]); }
        return e;
    }
    fcntl(ex[1], F_SETFD, FD_CLOEXEC);
    /* no pipe end may leak into the child beyond its 0/1/2 (dup2 clears
     * the flag on those), or the child would keep its own stdin open */
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 2; k++)
            if (p[i][k] >= 0) fcntl(p[i][k], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid == 0) {
        /* child: only async-signal-safe calls until exec */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        int devnull = -1;
        if (modes[0] == ZELL_PLAT_STDIO_NULL || modes[1] == ZELL_PLAT_STDIO_NULL || modes[2] == ZELL_PLAT_STDIO_NULL)
            devnull = open("/dev/null", O_RDWR);
        for (int i = 0; i < 3; i++) {
            if (modes[i] == ZELL_PLAT_STDIO_PIPE) dup2(p[i][i == 0 ? 0 : 1], i);
            else if (modes[i] == ZELL_PLAT_STDIO_NULL) dup2(devnull, i);
        }
        int e = 0;
        if (dir && chdir(dir) != 0) {
            e = -errno;  /* negative: chdir failed */
        } else {
            if (envp) exec_env(argv, envp); else execvp(argv[0], argv);
            e = errno;
        }
        ssize_t w = write(ex[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    int fork_errno = errno;
    close(ex[1]);
    if (p[0][0] >= 0) close(p[0][0]);
    if (p[1][1] >= 0) close(p[1][1]);
    if (p[2][1] >= 0) close(p[2][1]);
    int parent[3] = {p[0][1], p[1][0], p[2][0]};
    if (pid < 0) {
        close(ex[0]);
        for (int i = 0; i < 3; i++) if (parent[i] >= 0) close(parent[i]);
        *op = "fork";
        return fork_errno;
    }
    int child_errno = 0;
    ssize_t r;
    do r = read(ex[0], &child_errno, sizeof child_errno);
    while (r < 0 && errno == EINTR);
    close(ex[0]);
    if (r == (ssize_t)sizeof child_errno) {
        int st;
        waitpid(pid, &st, 0);
        for (int i = 0; i < 3; i++) if (parent[i] >= 0) close(parent[i]);
        *op = "exec";  /* a failed chdir in the child reports as exec, like before */
        return child_errno < 0 ? -child_errno : child_errno;
    }
    int pidfd = -1;
#if defined(SYS_pidfd_open)
    pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd >= 0) fcntl(pidfd, F_SETFD, FD_CLOEXEC);
#endif
    out->pid = pid;
    out->pidfd = pidfd;
    for (int i = 0; i < 3; i++) {
        if (parent[i] >= 0) nonblock(parent[i]);
        out->fds[i] = parent[i];
    }
    *op = NULL;
    return 0;
}

int64_t zell__plat_pipe_read(int64_t fd, void *buf, int64_t n) {
    for (;;) {
        ssize_t r = read((int)fd, buf, (size_t)n);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -ZELL_PLAT_EAGAIN;
        return -(int64_t)errno;
    }
}

int64_t zell__plat_pipe_write(int64_t fd, const void *buf, int64_t n) {
    for (;;) {
        ssize_t r = write((int)fd, buf, (size_t)n);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -ZELL_PLAT_EAGAIN;
        return -(int64_t)errno;
    }
}

void zell__plat_pipe_close(int64_t fd) {
    if (fd >= 0) close((int)fd);
}

int zell__plat_wait_pid(int64_t pid, int64_t *status) {
    int st;
    pid_t r;
    do r = waitpid((pid_t)pid, &st, WNOHANG);
    while (r < 0 && errno == EINTR);
    if (r == 0) return 0;
    if (r < 0) return -errno;
    if (WIFEXITED(st)) { *status = WEXITSTATUS(st); return 1; }
    if (WIFSIGNALED(st)) { *status = 1000 + WTERMSIG(st); return 1; }
    return 0;
}

int zell__plat_kill(int64_t pid, int sig) {
    if (kill((pid_t)pid, sig) != 0 && errno != ESRCH) return errno;
    return 0;
}

/* ---- signals: self-pipe + delivery thread ---------------------------------- */

#define NSIG_MAX 65

static int g_sig_pipe[2] = {-1, -1};
static void (*g_sig_deliver)(int);
static zell__plat_once g_sig_once = ZELL_PLAT_ONCE_INIT;

static void on_signal(int sig) {
    int saved = errno;
    unsigned char b = (unsigned char)sig;
    ssize_t r = write(g_sig_pipe[1], &b, 1);
    (void)r;
    errno = saved;
}

static void *sig_reader(void *arg) {
    (void)arg;
    /* this thread never takes signals itself */
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    for (;;) {
        unsigned char b;
        ssize_t n = read(g_sig_pipe[0], &b, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return NULL;
        if (b > 0 && b < NSIG_MAX) g_sig_deliver(b);
    }
}

static void sig_init(void) {
    if (pipe(g_sig_pipe) != 0) {
        fputs("signal: cannot create pipe\n", stderr);
        abort();
    }
    fcntl(g_sig_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_sig_pipe[1], F_SETFD, FD_CLOEXEC);
    if (!zell__plat_thread_start_detached(sig_reader, NULL)) {
        fputs("signal: cannot start thread\n", stderr);
        abort();
    }
}

void zell__plat_sig_start(void (*deliver)(int sig)) {
    g_sig_deliver = deliver;
    zell__plat_once_run(&g_sig_once, sig_init);
}

bool zell__plat_sig_catchable(int64_t sig) { return sig > 0 && sig < NSIG_MAX && sig != SIGKILL && sig != SIGSTOP; }

void zell__plat_sig_catch(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, NULL);
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, sig);
    pthread_sigmask(SIG_UNBLOCK, &one, NULL);
}

void zell__plat_sig_ignore(int sig) { signal(sig, SIG_IGN); }
void zell__plat_sig_default(int sig) { signal(sig, SIG_DFL); }
void zell__plat_sig_raise(int sig) { kill(getpid(), sig); }

/* ---- users (getpwuid_r & co., so NSS sources work too) ---------------------- */

int64_t zell__plat_uid(void) { return (int64_t)getuid(); }

int zell__plat_user_lookup(const char *name, int64_t uid, bool by_id, zell__plat_user *u) {
    struct passwd pw, *res = NULL;
    char buf[16384];
    int e = by_id ? getpwuid_r((uid_t)uid, &pw, buf, sizeof buf, &res) : getpwnam_r(name, &pw, buf, sizeof buf, &res);
    if (!res) return e ? e : -1;
    u->uid = pw.pw_uid;
    u->gid = pw.pw_gid;
    snprintf(u->name, sizeof u->name, "%s", pw.pw_name ? pw.pw_name : "");
    /* GECOS: the full name is the first comma-separated field */
    snprintf(u->full, sizeof u->full, "%s", pw.pw_gecos ? pw.pw_gecos : "");
    char *comma = strchr(u->full, ',');
    if (comma) *comma = 0;
    snprintf(u->home, sizeof u->home, "%s", pw.pw_dir ? pw.pw_dir : "");
    return 0;
}

int zell__plat_group_lookup(const char *name, int64_t gid, bool by_id, int64_t *gid_out, char *gname, size_t cap) {
    struct group gr, *res = NULL;
    char buf[16384];
    int e = by_id ? getgrgid_r((gid_t)gid, &gr, buf, sizeof buf, &res) : getgrnam_r(name, &gr, buf, sizeof buf, &res);
    if (!res) return e ? e : -1;
    *gid_out = gr.gr_gid;
    snprintf(gname, cap, "%s", gr.gr_name ? gr.gr_name : "");
    return 0;
}

#else
/* Windows: fs_win32.c, proc_win32.c, win32.c. */
typedef int zell__fs_posix_unused;
#endif
