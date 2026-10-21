/* platform.h - the runtime's internal platform layer (docs/PLATFORMS.md).
 *
 * Everything OS-specific that the rest of the runtime needs goes through
 * these functions; the implementations live in runtime/src/platform/:
 *   posix.c        threads, CPU count, monotonic clock, entropy, backtraces
 *   poll_epoll.c   readiness poller on Linux (epoll + eventfd)
 *   poll_kqueue.c  readiness poller on macOS/BSD (kqueue + EVFILT_USER)
 *   net_posix.c    BSD sockets (TCP/UDP/Unix, options, sendfile), names
 *   fs_posix.c     files, directories, environment, child processes,
 *                  signals, users (POSIX)
 *   win32.c        Windows: threads, locks, clocks, entropy, backtraces,
 *                  dynamic loading, console, environment, signals
 *   fs_win32.c     Windows files and directories (UTF-16 at the boundary)
 *   proc_win32.c   Windows child processes (CreateProcessW)
 *   poll_wsapoll.c Windows readiness poller (WSAPoll; IOCP planned)
 *   net_win32.c    Winsock2
 *   timefmt.c      strftime/strptime where the C library lacks them
 * Not part of the ABI: every symbol is hidden and prefixed slang__plat_
 * (types: slang__plat_mutex, slang__plat_cond, ...). This header and the
 * files above are the only places with OS conditionals (#ifdef _WIN32,
 * __linux__, __APPLE__); the rest of the runtime is OS-independent. */
#ifndef SLANG_PLATFORM_H
#define SLANG_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#if defined(_WIN32)
/* PE/COFF has no ELF visibility; symbols of a static archive are not
 * exported from the executable anyway. */
#define SLANG_PLAT_HIDDEN
#else
#define SLANG_PLAT_HIDDEN __attribute__((visibility("hidden")))
#endif

/* ---- threads ---------------------------------------------------------- */

typedef void *(*slang__plat_thread_fn)(void *arg);
typedef struct slang__plat_thread { uintptr_t handle; } slang__plat_thread;

/* Starts a joinable thread; returns false on failure. */
SLANG_PLAT_HIDDEN bool slang__plat_thread_start(slang__plat_thread *t, slang__plat_thread_fn fn, void *arg);
/* Starts a detached thread; returns false on failure. */
SLANG_PLAT_HIDDEN bool slang__plat_thread_start_detached(slang__plat_thread_fn fn, void *arg);
SLANG_PLAT_HIDDEN void slang__plat_thread_join(slang__plat_thread t);
/* Number of online CPUs (at least 1). */
SLANG_PLAT_HIDDEN int64_t slang__plat_cpu_count(void);
/* Gives up the CPU (spin-lock back-off). */
SLANG_PLAT_HIDDEN void slang__plat_yield(void);

/* ---- mutexes, condition variables, once, thread-local keys ------------
 * pthreads on POSIX (inline), SRWLOCK / CONDITION_VARIABLE / INIT_ONCE /
 * FLS on Windows (win32.c). Mutexes are not recursive. Static
 * initialisers: SLANG_PLAT_MUTEX_INIT, SLANG_PLAT_COND_INIT,
 * SLANG_PLAT_ONCE_INIT. */

#if defined(_WIN32)
typedef struct slang__plat_mutex { void *p; } slang__plat_mutex;  /* SRWLOCK */
typedef struct slang__plat_cond { void *p; } slang__plat_cond;    /* CONDITION_VARIABLE */
typedef struct slang__plat_once { void *p; } slang__plat_once;    /* INIT_ONCE */
typedef struct slang__plat_tls_key { unsigned long idx; } slang__plat_tls_key;  /* FLS index */
#define SLANG_PLAT_MUTEX_INIT {0}
#define SLANG_PLAT_COND_INIT {0}
#define SLANG_PLAT_ONCE_INIT {0}
SLANG_PLAT_HIDDEN void slang__plat_mutex_init(slang__plat_mutex *m);
SLANG_PLAT_HIDDEN void slang__plat_mutex_destroy(slang__plat_mutex *m);
SLANG_PLAT_HIDDEN void slang__plat_mutex_lock(slang__plat_mutex *m);
SLANG_PLAT_HIDDEN void slang__plat_mutex_unlock(slang__plat_mutex *m);
SLANG_PLAT_HIDDEN void slang__plat_cond_init(slang__plat_cond *c);
SLANG_PLAT_HIDDEN void slang__plat_cond_destroy(slang__plat_cond *c);
SLANG_PLAT_HIDDEN void slang__plat_cond_wait(slang__plat_cond *c, slang__plat_mutex *m);
SLANG_PLAT_HIDDEN bool slang__plat_cond_wait_ns(slang__plat_cond *c, slang__plat_mutex *m, int64_t ns);
SLANG_PLAT_HIDDEN void slang__plat_cond_signal(slang__plat_cond *c);
SLANG_PLAT_HIDDEN void slang__plat_cond_broadcast(slang__plat_cond *c);
SLANG_PLAT_HIDDEN void slang__plat_once_run(slang__plat_once *o, void (*fn)(void));
SLANG_PLAT_HIDDEN bool slang__plat_tls_new(slang__plat_tls_key *k, void (*dtor)(void *));
SLANG_PLAT_HIDDEN void slang__plat_tls_set(slang__plat_tls_key k, void *v);
#else
#include <errno.h>
#include <pthread.h>
typedef pthread_mutex_t slang__plat_mutex;
/* `mono`: created by slang__plat_cond_init, whose timed waits use
 * CLOCK_MONOTONIC where the system allows it (not macOS). */
typedef struct slang__plat_cond { pthread_cond_t c; int mono; } slang__plat_cond;
typedef pthread_once_t slang__plat_once;
typedef struct slang__plat_tls_key { pthread_key_t k; } slang__plat_tls_key;
#define SLANG_PLAT_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define SLANG_PLAT_COND_INIT {PTHREAD_COND_INITIALIZER, 0}
#define SLANG_PLAT_ONCE_INIT PTHREAD_ONCE_INIT
static inline void slang__plat_mutex_init(slang__plat_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void slang__plat_mutex_destroy(slang__plat_mutex *m) { pthread_mutex_destroy(m); }
static inline void slang__plat_mutex_lock(slang__plat_mutex *m) { pthread_mutex_lock(m); }
static inline void slang__plat_mutex_unlock(slang__plat_mutex *m) { pthread_mutex_unlock(m); }
static inline void slang__plat_cond_init(slang__plat_cond *c) {
#if defined(__APPLE__)
    pthread_cond_init(&c->c, NULL);
    c->mono = 0;
#else
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    c->mono = pthread_condattr_setclock(&a, CLOCK_MONOTONIC) == 0;
    pthread_cond_init(&c->c, &a);
    pthread_condattr_destroy(&a);
#endif
}
static inline void slang__plat_cond_destroy(slang__plat_cond *c) { pthread_cond_destroy(&c->c); }
static inline void slang__plat_cond_wait(slang__plat_cond *c, slang__plat_mutex *m) { pthread_cond_wait(&c->c, m); }
/* Waits at most ns nanoseconds; false on timeout. */
static inline bool slang__plat_cond_wait_ns(slang__plat_cond *c, slang__plat_mutex *m, int64_t ns) {
    if (ns < 0) ns = 0;
    struct timespec ts;
    clock_gettime(c->mono ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    int64_t t = (int64_t)ts.tv_nsec + ns % 1000000000;
    ts.tv_sec += (time_t)(ns / 1000000000 + t / 1000000000);
    ts.tv_nsec = (long)(t % 1000000000);
    return pthread_cond_timedwait(&c->c, m, &ts) != ETIMEDOUT;
}
static inline void slang__plat_cond_signal(slang__plat_cond *c) { pthread_cond_signal(&c->c); }
static inline void slang__plat_cond_broadcast(slang__plat_cond *c) { pthread_cond_broadcast(&c->c); }
static inline void slang__plat_once_run(slang__plat_once *o, void (*fn)(void)) { pthread_once(o, fn); }
static inline bool slang__plat_tls_new(slang__plat_tls_key *k, void (*dtor)(void *)) { return pthread_key_create(&k->k, dtor) == 0; }
static inline void slang__plat_tls_set(slang__plat_tls_key k, void *v) { pthread_setspecific(k.k, v); }
#endif

/* stdio locking around a line of output (flockfile & co. on POSIX,
 * _lock_file on Windows). */
#if defined(_WIN32)
#define slang__plat_flockfile(f) _lock_file(f)
#define slang__plat_funlockfile(f) _unlock_file(f)
#else
#define slang__plat_flockfile(f) flockfile(f)
#define slang__plat_funlockfile(f) funlockfile(f)
#endif

/* ---- memory ------------------------------------------------------------- */

/* Aligned allocation (align a power of two, size a multiple of it); must
 * be released with slang__plat_aligned_free. */
SLANG_PLAT_HIDDEN void *slang__plat_aligned_alloc(size_t align, size_t size);
SLANG_PLAT_HIDDEN void slang__plat_aligned_free(void *p);
/* Usable size of a malloc'd block (malloc_usable_size / malloc_size / _msize). */
SLANG_PLAT_HIDDEN size_t slang__plat_alloc_size(void *p);

/* ---- time and entropy ------------------------------------------------- */

/* Monotonic clock in nanoseconds (arbitrary origin). */
SLANG_PLAT_HIDDEN int64_t slang__plat_mono_ns(void);
/* Wall clock: nanoseconds since the Unix epoch. */
SLANG_PLAT_HIDDEN int64_t slang__plat_wall_ns(void);
/* Sleeps the calling thread (at least ns nanoseconds). */
SLANG_PLAT_HIDDEN void slang__plat_sleep_ns(int64_t ns);
/* Calendar fields of Unix second t in UTC (gmtime_r, proleptic Gregorian,
 * any int64 year range the struct holds) and the inverse (timegm, which
 * normalises out-of-range fields like mktime). Pure C, timefmt.c. */
SLANG_PLAT_HIDDEN void slang__plat_utc_tm(int64_t t, struct tm *tm);
SLANG_PLAT_HIDDEN int64_t slang__plat_timegm(const struct tm *tm);
/* strftime with the C99/POSIX conversions on every platform (the C library's
 * own where it has them all, timefmt.c otherwise); 0 if cap is too small. */
SLANG_PLAT_HIDDEN size_t slang__plat_strftime(char *buf, size_t cap, const char *fmt, const struct tm *tm);
/* strptime; *gmtoff gets the offset parsed by %z (seconds east), else 0.
 * Returns the end of the parsed input or NULL. */
SLANG_PLAT_HIDDEN const char *slang__plat_strptime(const char *s, const char *fmt, struct tm *tm, long *gmtoff);
/* The "C"-locale implementations behind them on Windows (timefmt.c,
 * compiled everywhere so POSIX builds test them against the C library). */
SLANG_PLAT_HIDDEN size_t slang__plat_strftime_portable(char *buf, size_t cap, const char *fmt, const struct tm *tm);
SLANG_PLAT_HIDDEN const char *slang__plat_strptime_portable(const char *s, const char *fmt, struct tm *tm, long *gmtoff);
/* The local zone's TZif file when $TZ is unset ("/etc/localtime"), and the
 * default zoneinfo directory; NULL where the system has none (Windows:
 * UTC unless $TZ and $ZONEINFO name a TZif file). */
SLANG_PLAT_HIDDEN const char *slang__plat_localtime_path(void);
SLANG_PLAT_HIDDEN const char *slang__plat_zoneinfo_dir(void);
/* Fills buf with n cryptographically secure random bytes; false on failure. */
SLANG_PLAT_HIDDEN bool slang__plat_random_bytes(void *buf, size_t n);

/* ---- diagnostics ------------------------------------------------------ */

/* Writes a best-effort backtrace of the calling thread to fd, skipping the
 * innermost `skip` frames. */
SLANG_PLAT_HIDDEN void slang__plat_backtrace(int fd, int skip);

/* ---- CPU profiling (posix.c; std.diag, runtime/src/profile.c) ---------
 * A process-wide CPU-time timer (SIGPROF on POSIX) calls `fn` from the
 * signal handler of whichever thread was running, with the interrupted PC
 * first and then the return addresses found by walking frame pointers
 * (only within the thread's stack bounds recorded by
 * slang__plat_thread_stack_init; other threads report the PC alone). `fn`
 * must be async-signal-safe. */
typedef void (*slang__plat_prof_fn)(const uintptr_t *pcs, int n);
/* Records the calling thread's stack bounds for frame walks (cheap; called
 * by worker threads at start and by the profiler for the caller). */
SLANG_PLAT_HIDDEN void slang__plat_thread_stack_init(void);
/* Starts sampling `hz` times per CPU second; false if unsupported. */
SLANG_PLAT_HIDDEN bool slang__plat_prof_start(int hz, slang__plat_prof_fn fn);
SLANG_PLAT_HIDDEN void slang__plat_prof_stop(void);
/* Symbolizes a code address of the process: writes the function name to
 * buf (demangling nothing) and its start address to *start; false if not
 * found. Uses the executable's own symbol table (local symbols included),
 * then the dynamic linker. Not async-signal-safe. */
SLANG_PLAT_HIDDEN bool slang__plat_symbolize(uintptr_t pc, char *buf, size_t cap, uintptr_t *start);
/* Address range and path of the main executable's text (for pprof
 * mappings); false if unknown. */
SLANG_PLAT_HIDDEN bool slang__plat_exe_range(uintptr_t *lo, uintptr_t *hi, char *path, size_t cap);
/* Drops the symbol table cache. */
SLANG_PLAT_HIDDEN void slang__plat_symbolize_done(void);

/* ---- dynamic loading (std.plugin, posix.c) ---------------------------- */

/* Opens a shared object (symbols bound now, kept local); NULL on failure. */
SLANG_PLAT_HIDDEN void *slang__plat_dl_open(const char *path);
/* Address of `name` in an opened object, NULL if absent. */
SLANG_PLAT_HIDDEN void *slang__plat_dl_sym(void *h, const char *name);
SLANG_PLAT_HIDDEN void slang__plat_dl_close(void *h);
/* The last loader error (static text). */
SLANG_PLAT_HIDDEN const char *slang__plat_dl_error(void);

/* ---- readiness poller (async I/O backend) -----------------------------
 * A readiness poller is level-triggered and one-shot: poller_arm(fd,
 * interest, token) asks for ONE report when fd is ready for any direction
 * in `interest`; after that report (or a spurious one) the fd stays
 * registered but disarmed until it is armed again. One-shot level
 * triggering gives the correctness of level triggering (data that is
 * already there is reported at once, nothing is ever lost between a read
 * that saw EAGAIN and the arm) with one system call per wait. Reports may
 * be spurious (a direction nobody asked for, readiness that is gone by the
 * time the caller looks): callers retry the operation and wait again.
 * Every function is thread-safe; arm/remove may run while another thread
 * is inside poller_wait. Backends: poll_epoll.c (Linux), poll_kqueue.c
 * (macOS/BSD); a completion-based backend (IOCP) is adapted behind the
 * same interface, see docs/PLATFORMS.md. */

enum { SLANG_PLAT_READ = 1, SLANG_PLAT_WRITE = 2, SLANG_PLAT_ERROR = 4 };

/* Token of the poller's own wake-up channel in events. */
#define SLANG_PLAT_WAKE_TOKEN UINT64_MAX

typedef struct slang__plat_event {
    uint64_t token;  /* the token given to poller_arm, or SLANG_PLAT_WAKE_TOKEN */
    int32_t events;  /* SLANG_PLAT_READ | SLANG_PLAT_WRITE | SLANG_PLAT_ERROR */
} slang__plat_event;

typedef struct slang__plat_poller slang__plat_poller;

/* A new poller with a wake-up channel; NULL on failure. */
SLANG_PLAT_HIDDEN slang__plat_poller *slang__plat_poller_new(void);
SLANG_PLAT_HIDDEN void slang__plat_poller_free(slang__plat_poller *p);
/* Arms fd for one report of `interest` (READ/WRITE mask) carrying `token`.
 * `registered` says whether fd was already added to this poller (the
 * function copes with a wrong guess). interest 0 disarms. Returns 0 or an
 * errno: SLANG_PLAT_ENOTPOLLABLE for a descriptor that cannot be polled
 * (a regular file: always ready), EBADF for a closed one. */
#define SLANG_PLAT_ENOTPOLLABLE 100001
SLANG_PLAT_HIDDEN int slang__plat_poller_arm(slang__plat_poller *p, int64_t fd, int32_t interest, uint64_t token, bool registered);
/* Removes fd (before it is closed). */
SLANG_PLAT_HIDDEN void slang__plat_poller_remove(slang__plat_poller *p, int64_t fd);
/* Waits up to timeout_ms (-1 = forever) and stores up to max events.
 * Returns the number of events (0 on timeout or EINTR). The wake-up
 * channel shows up as SLANG_PLAT_WAKE_TOKEN and is drained internally. */
SLANG_PLAT_HIDDEN int slang__plat_poller_wait(slang__plat_poller *p, slang__plat_event *evs, int max, int64_t timeout_ms);
/* Makes a concurrent or the next poller_wait return (thread-safe). */
SLANG_PLAT_HIDDEN void slang__plat_poller_wake(slang__plat_poller *p);

/* ---- process, console, environment (posix.c / win32.c) ---------------- */

/* Once, from slang_runtime_init before anything else: on Windows replaces
 * *argv by the UTF-8 command line (CommandLineToArgvW), puts stdout/stderr
 * in binary mode (println writes \n) and switches an attached console to
 * UTF-8 with ANSI escape processing. No-op on POSIX. */
SLANG_PLAT_HIDDEN void slang__plat_runtime_init(int *argc, char ***argv);
/* Writes n bytes to a stdio stream: fwrite, except that on a Windows
 * console the UTF-8 text goes through WriteConsoleW (a sequence split
 * across calls is carried over). Caller holds the stream lock or not. */
SLANG_PLAT_HIDDEN void slang__plat_stream_write(FILE *f, const char *p, size_t n);
SLANG_PLAT_HIDDEN int64_t slang__plat_pid(void);
/* malloc'd UTF-8 value of an environment variable, NULL if unset. */
SLANG_PLAT_HIDDEN char *slang__plat_getenv(const char *name);
/* setenv / unsetenv (value NULL); 0 or an errno. */
SLANG_PLAT_HIDDEN int slang__plat_setenv(const char *name, const char *value);
/* A malloc'd NULL-terminated copy of the environment ("K=V" UTF-8
 * strings, each malloc'd); slang__plat_free_strv releases it. */
SLANG_PLAT_HIDDEN char **slang__plat_environ(void);
SLANG_PLAT_HIDDEN void slang__plat_free_strv(char **v);
/* Host name into buf; 0 or an errno. */
SLANG_PLAT_HIDDEN int slang__plat_hostname(char *buf, size_t cap);
/* malloc'd directory for temporary files ($TMPDIR or /tmp; GetTempPathW). */
SLANG_PLAT_HIDDEN char *slang__plat_temp_dir(void);
/* Path separator of the platform ('/' or '\\'). */
SLANG_PLAT_HIDDEN char slang__plat_path_sep(void);

/* ---- files and directories (fs_posix.c / fs_win32.c) --------------------
 * Descriptors are int64_t (POSIX fds; C runtime fds over Win32 handles on
 * Windows, so 0/1/2 are the standard streams everywhere). Paths are UTF-8
 * (UTF-16 at the Win32 boundary, where '/' and '\\' both separate).
 * Functions returning int64_t give a value >= 0 or -errno; int ones 0 or an
 * errno (POSIX errno values on every platform, so strerror and
 * slang_error_errno work unchanged). EINTR is retried internally. */

/* Open flags: the portable bits of std.os (os.sl). */
enum {
    SLANG_PLAT_O_RDONLY = 0, SLANG_PLAT_O_WRONLY = 1, SLANG_PLAT_O_RDWR = 2, SLANG_PLAT_O_APPEND = 8,
    SLANG_PLAT_O_CREATE = 64, SLANG_PLAT_O_EXCL = 128, SLANG_PLAT_O_SYNC = 256, SLANG_PLAT_O_TRUNC = 512,
};
enum { SLANG_PLAT_KIND_FILE = 0, SLANG_PLAT_KIND_DIR = 1, SLANG_PLAT_KIND_LINK = 2, SLANG_PLAT_KIND_OTHER = 3 };
typedef struct slang__plat_stat {
    int64_t size;
    int64_t mtime_sec;
    int32_t mtime_nsec;
    uint32_t perm;   /* permission bits (07777) */
    int32_t kind;    /* SLANG_PLAT_KIND_* */
} slang__plat_stat;

/* Opens (close-on-exec / not inherited); perm applies when creating. */
SLANG_PLAT_HIDDEN int64_t slang__plat_open(const char *path, int flags, int perm);
SLANG_PLAT_HIDDEN int64_t slang__plat_read(int64_t fd, void *buf, int64_t n);
SLANG_PLAT_HIDDEN int64_t slang__plat_pread(int64_t fd, void *buf, int64_t n, int64_t off);
/* One write (may be short). */
SLANG_PLAT_HIDDEN int64_t slang__plat_write(int64_t fd, const void *buf, int64_t n);
/* Writes everything; 0 or an errno. */
SLANG_PLAT_HIDDEN int slang__plat_write_all(int64_t fd, const void *buf, int64_t n);
/* whence: 0 set, 1 current, 2 end. */
SLANG_PLAT_HIDDEN int64_t slang__plat_seek(int64_t fd, int64_t off, int whence);
SLANG_PLAT_HIDDEN int slang__plat_close(int64_t fd);
SLANG_PLAT_HIDDEN int slang__plat_fsync(int64_t fd);
SLANG_PLAT_HIDDEN int slang__plat_ftruncate(int64_t fd, int64_t size);
SLANG_PLAT_HIDDEN int slang__plat_fstat(int64_t fd, slang__plat_stat *st);
/* stat (follow) or lstat. */
SLANG_PLAT_HIDDEN int slang__plat_stat_path(const char *path, bool follow, slang__plat_stat *st);
SLANG_PLAT_HIDDEN bool slang__plat_isatty(int64_t fd);
SLANG_PLAT_HIDDEN int slang__plat_mkdir(const char *path);
SLANG_PLAT_HIDDEN int slang__plat_rmdir(const char *path);
SLANG_PLAT_HIDDEN int slang__plat_unlink_file(const char *path);
/* remove(3): a file or an empty directory. */
SLANG_PLAT_HIDDEN int slang__plat_remove(const char *path);
/* Replaces an existing destination (like POSIX rename). */
SLANG_PLAT_HIDDEN int slang__plat_rename(const char *from, const char *to);
SLANG_PLAT_HIDDEN int slang__plat_chmod(const char *path, int mode);
/* malloc'd current directory, NULL with errno-style *err. */
SLANG_PLAT_HIDDEN char *slang__plat_getcwd(int *err);
SLANG_PLAT_HIDDEN int slang__plat_chdir(const char *path);
/* A regular file the current user may execute (on Windows: any regular
 * file, executability is by extension). */
SLANG_PLAT_HIDDEN bool slang__plat_is_executable(const char *path);
/* Directory listing without "." and "..". dir_next returns the next name
 * (valid until the next call) and its kind, or -1 if unknown (callers
 * stat the entry); NULL at the end or on error (*err set, 0 at the end). */
typedef struct slang__plat_dir slang__plat_dir;
SLANG_PLAT_HIDDEN slang__plat_dir *slang__plat_dir_open(const char *path, int *err);
SLANG_PLAT_HIDDEN const char *slang__plat_dir_next(slang__plat_dir *d, int *kind, int *err);
SLANG_PLAT_HIDDEN void slang__plat_dir_close(slang__plat_dir *d);

/* ---- child processes (fs_posix.c / proc_win32.c) ------------------------ */

enum { SLANG_PLAT_STDIO_INHERIT = 0, SLANG_PLAT_STDIO_PIPE = 1, SLANG_PLAT_STDIO_NULL = 2 };
typedef struct slang__plat_child {
    int64_t pid;
    int64_t pidfd;   /* pollable exit notification (Linux pidfd) or -1 */
    int64_t fds[3];  /* parent ends of stdin/stdout/stderr pipes or -1: non-blocking, pollable */
} slang__plat_child;
/* Starts argv[0] (searched in PATH) with argv, envp (NULL = inherit) in
 * dir (NULL = current) with the stdio modes. 0 or an errno; *op names the
 * failing step ("pipe", "fork", "exec"). */
SLANG_PLAT_HIDDEN int slang__plat_spawn(char *const *argv, char *const *envp, const char *dir, const int modes[3],
                                        slang__plat_child *out, const char **op);
/* Non-blocking pipe I/O on the parent ends: bytes, 0 at EOF (read),
 * -SLANG_PLAT_EAGAIN, or -errno. */
SLANG_PLAT_HIDDEN int64_t slang__plat_pipe_read(int64_t fd, void *buf, int64_t n);
SLANG_PLAT_HIDDEN int64_t slang__plat_pipe_write(int64_t fd, const void *buf, int64_t n);
SLANG_PLAT_HIDDEN void slang__plat_pipe_close(int64_t fd);
/* Reaps without blocking: 1 with *status = exit code (or 1000 + signal),
 * 0 while running, or -errno. */
SLANG_PLAT_HIDDEN int slang__plat_wait_pid(int64_t pid, int64_t *status);
/* Sends sig (TerminateProcess on Windows); 0 (also for a gone process) or an errno. */
SLANG_PLAT_HIDDEN int slang__plat_kill(int64_t pid, int sig);

/* ---- signals (fs_posix.c / win32.c) -------------------------------------
 * Signal numbers are the POSIX ones (SIGINT 2, SIGTERM 15, ...). On
 * Windows SetConsoleCtrlHandler maps Ctrl+C / Ctrl+Break to SIGINT and
 * close/logoff/shutdown to SIGTERM; other signals cannot be caught. */

/* Starts the delivery thread once: `deliver(sig)` runs on an ordinary
 * thread (not in a signal handler) for every caught signal. */
SLANG_PLAT_HIDDEN void slang__plat_sig_start(void (*deliver)(int sig));
/* True if sig can be caught (not KILL/STOP, in range). */
SLANG_PLAT_HIDDEN bool slang__plat_sig_catchable(int64_t sig);
/* Installs the catching handler / SIG_IGN / SIG_DFL for sig. */
SLANG_PLAT_HIDDEN void slang__plat_sig_catch(int sig);
SLANG_PLAT_HIDDEN void slang__plat_sig_ignore(int sig);
SLANG_PLAT_HIDDEN void slang__plat_sig_default(int sig);
/* Sends sig to the process itself. */
SLANG_PLAT_HIDDEN void slang__plat_sig_raise(int sig);

/* ---- users (fs_posix.c; Windows: the current user only) ----------------- */

typedef struct slang__plat_user {
    int64_t uid, gid;
    char name[256], full[512], home[1024];
} slang__plat_user;
/* 0 found, -1 not found, else an errno. */
SLANG_PLAT_HIDDEN int slang__plat_user_lookup(const char *name, int64_t uid, bool by_id, slang__plat_user *u);
/* group name of gid / gid of name: 0 found, -1 not found, else an errno. */
SLANG_PLAT_HIDDEN int slang__plat_group_lookup(const char *name, int64_t gid, bool by_id, int64_t *gid_out, char *gname, size_t cap);
/* Real user id (-1 where there is none: Windows). */
SLANG_PLAT_HIDDEN int64_t slang__plat_uid(void);

/* ---- process setup ------------------------------------------------------ */

/* Once per process (scheduler start): ignores SIGPIPE (a write to a peer
 * that went away reports EPIPE instead of killing the process; sockets use
 * MSG_NOSIGNAL/SO_NOSIGPIPE as well) and raises the soft open-files limit
 * to the hard one, like Go and Rust do. */
SLANG_PLAT_HIDDEN void slang__plat_process_init(void);

/* ---- sockets (net_posix.c) --------------------------------------------
 * Descriptors are int64_t. Functions returning int64_t give a value >= 0 or
 * -errno; SLANG_PLAT_EAGAIN is the (negated) "would block" code (also
 * "connect in progress"). All sockets are non-blocking and close-on-exec,
 * and never raise SIGPIPE. Errno-style codes are the host's; the
 * SLANG_PLAT_E* classes below name the ones std.net maps to sentinels. */

#define SLANG_PLAT_EAGAIN 100000  /* returned as -SLANG_PLAT_EAGAIN (not an errno value) */

/* Error classes of an errno-style code (slang__plat_err_class). */
enum {
    SLANG_PLAT_E_OTHER = 0, SLANG_PLAT_E_REFUSED, SLANG_PLAT_E_RESET, SLANG_PLAT_E_UNREACHABLE,
    SLANG_PLAT_E_ADDRINUSE, SLANG_PLAT_E_TIMEOUT, SLANG_PLAT_E_CLOSED, SLANG_PLAT_E_INVAL,
};
SLANG_PLAT_HIDDEN int slang__plat_err_class(int code);
/* Lower-case message for an errno-style code ("connection refused"). */
SLANG_PLAT_HIDDEN const char *slang__plat_strerror(int code, char *buf, size_t cap);

/* A socket address (IPv4, IPv6 with scope id, or Unix domain path). */
enum { SLANG_PLAT_AF_INET = 4, SLANG_PLAT_AF_INET6 = 6, SLANG_PLAT_AF_UNIX = 1 };
typedef struct slang__plat_addr {
    union { uint64_t align; unsigned char bytes[128]; } u;  /* struct sockaddr_storage */
    uint32_t len;
} slang__plat_addr;

/* An IPv4 (4 bytes) or IPv6 (16 bytes) address with port and IPv6 scope. */
SLANG_PLAT_HIDDEN bool slang__plat_addr_ip(slang__plat_addr *a, const unsigned char *ip, int iplen, int64_t port, uint32_t scope);
/* A Unix domain socket path; "@name" is the Linux abstract namespace.
 * False if the path is too long (or abstract names are unsupported). */
SLANG_PLAT_HIDDEN bool slang__plat_addr_unix(slang__plat_addr *a, const char *path);
/* SLANG_PLAT_AF_* of a, or 0. */
SLANG_PLAT_HIDDEN int slang__plat_addr_family(const slang__plat_addr *a);
/* Parts of an IP address: bytes (4 or 16; *iplen), port, scope. */
SLANG_PLAT_HIDDEN void slang__plat_addr_parts(const slang__plat_addr *a, unsigned char ip[16], int *iplen, int64_t *port, uint32_t *scope);
/* "ip:port", "[ip%zone]:port" or the Unix path ("@name" for abstract,
 * "" for unnamed). */
SLANG_PLAT_HIDDEN void slang__plat_addr_format(const slang__plat_addr *a, char *buf, size_t cap);
/* Numeric IP text (no brackets; IPv4-mapped IPv6 keeps the ::ffff: form)
 * to 4 or 16 bytes; returns the length or 0. */
SLANG_PLAT_HIDDEN int slang__plat_parse_ip(const char *s, unsigned char out[16]);
/* Formats 4 or 16 bytes; false for another length. */
SLANG_PLAT_HIDDEN bool slang__plat_format_ip(const unsigned char *ip, int len, char *buf, size_t cap);
/* Interface name <-> index (zones of link-local IPv6 addresses); 0 / false if unknown. */
SLANG_PLAT_HIDDEN uint32_t slang__plat_if_index(const char *name);
SLANG_PLAT_HIDDEN bool slang__plat_if_name(uint32_t index, char *buf, size_t cap);
/* True if s is a numeric IPv4 or IPv6 address. */
SLANG_PLAT_HIDDEN bool slang__plat_is_ip(const char *s);

enum { SLANG_PLAT_STREAM = 1, SLANG_PLAT_DGRAM = 2 };
/* A new non-blocking socket of family SLANG_PLAT_AF_* and type. */
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_open(int family, int type);
/* Starts a connect: 0 when connected, -SLANG_PLAT_EAGAIN when in progress
 * (wait for writability, then sock_error), -errno on failure. */
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_connect(int64_t fd, const slang__plat_addr *a);
/* SO_ERROR (0 or errno). */
SLANG_PLAT_HIDDEN int slang__plat_sock_error(int64_t fd);
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_bind(int64_t fd, const slang__plat_addr *a);
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_listen(int64_t fd, int backlog);
/* A non-blocking connection and its peer address (from may be NULL). */
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_accept(int64_t fd, slang__plat_addr *from);
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_recv(int64_t fd, void *buf, int64_t n);  /* 0 = EOF */
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_send(int64_t fd, const void *buf, int64_t n);
typedef struct slang__plat_iovec { const void *base; size_t len; } slang__plat_iovec;
/* Gathered send of up to 64 buffers. */
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_sendv(int64_t fd, const slang__plat_iovec *iov, int n);
/* Datagrams. *truncated is set when the datagram was longer than n (its
 * rest is discarded); from may be NULL. */
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_recvfrom(int64_t fd, void *buf, int64_t n, slang__plat_addr *from, bool *truncated);
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_sendto(int64_t fd, const void *buf, int64_t n, const slang__plat_addr *to);
/* shutdown(2): how = SLANG_PLAT_READ, SLANG_PLAT_WRITE or both. */
SLANG_PLAT_HIDDEN int slang__plat_sock_shutdown(int64_t fd, int how);
SLANG_PLAT_HIDDEN void slang__plat_sock_close(int64_t fd);
/* Local (peer = false) or remote address; false on error. */
SLANG_PLAT_HIDDEN bool slang__plat_sock_name(int64_t fd, bool peer, slang__plat_addr *a);

/* Socket options (sock_setopt): value is a flag (0/1) or a number. */
enum {
    SLANG_PLAT_OPT_NODELAY = 1, SLANG_PLAT_OPT_KEEPALIVE, SLANG_PLAT_OPT_KEEPIDLE, SLANG_PLAT_OPT_KEEPINTVL,
    SLANG_PLAT_OPT_KEEPCNT, SLANG_PLAT_OPT_RCVBUF, SLANG_PLAT_OPT_SNDBUF, SLANG_PLAT_OPT_LINGER,
    SLANG_PLAT_OPT_REUSEADDR, SLANG_PLAT_OPT_REUSEPORT, SLANG_PLAT_OPT_BROADCAST, SLANG_PLAT_OPT_CORK,
    SLANG_PLAT_OPT_V6ONLY, SLANG_PLAT_OPT_FASTOPEN, SLANG_PLAT_OPT_MCAST_LOOP, SLANG_PLAT_OPT_MCAST_TTL,
    SLANG_PLAT_OPT_MCAST_IF,  /* value: interface index (0 = default) */
};
/* Returns 0 or an errno (ENOPROTOOPT where the platform lacks the option).
 * LINGER: value < 0 restores the default, >= 0 lingers that many seconds
 * (0 = reset on close). */
SLANG_PLAT_HIDDEN int slang__plat_sock_setopt(int64_t fd, int opt, int64_t value);
/* Reads back RCVBUF/SNDBUF/NODELAY/KEEPALIVE...; -errno on error. */
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_getopt(int64_t fd, int opt);
/* Joins (join = true) or leaves a multicast group on interface ifindex (0 = default). */
SLANG_PLAT_HIDDEN int slang__plat_sock_mcast(int64_t fd, bool join, const unsigned char *group, int len, uint32_t ifindex);

/* Files for sendfile: a read-only descriptor, its size; close. */
SLANG_PLAT_HIDDEN int64_t slang__plat_file_open(const char *path);
SLANG_PLAT_HIDDEN int64_t slang__plat_file_size(int64_t fd);
SLANG_PLAT_HIDDEN void slang__plat_file_close(int64_t fd);
/* Sends up to n bytes of file fd from offset off to socket sock:
 * sendfile(2) on Linux and the BSDs, a pread+send loop elsewhere. Bytes
 * sent (0 at end of file) or -errno / -SLANG_PLAT_EAGAIN. */
SLANG_PLAT_HIDDEN int64_t slang__plat_sendfile(int64_t sock, int64_t fd, int64_t off, int64_t n);
/* unlink(2) of a Unix socket path (not for abstract names). */
SLANG_PLAT_HIDDEN void slang__plat_unlink(const char *path);

/* ---- name resolution (blocking; std_net.c runs these on its resolver pool) */

enum { SLANG_PLAT_RESOLVE_V4FIRST = 1, SLANG_PLAT_RESOLVE_PASSIVE = 2 };
/* getaddrinfo: on success *out is a malloc'd array of n malloc'd numeric
 * address strings (with "%zone" for scoped IPv6), in the system's
 * preference order (RFC 6724) or IPv4 first with RESOLVE_V4FIRST, without
 * duplicates; returns NULL, else an error message (static storage) and
 * *notfound tells whether the name does not exist. */
SLANG_PLAT_HIDDEN const char *slang__plat_resolve(const char *host, int flags, char ***out, int *n, bool *notfound);
/* Reverse lookup (getnameinfo) of a numeric address: one name. */
SLANG_PLAT_HIDDEN const char *slang__plat_reverse(const char *ip, char ***out, int *n, bool *notfound);
/* Port of a service name ("http") for network "tcp"/"udp"; -1 if unknown. */
SLANG_PLAT_HIDDEN int64_t slang__plat_lookup_port(const char *network, const char *service);
/* Network interfaces: malloc'd lines "name\tindex\tmtu\tflags\taddr/prefix,addr/prefix"
 * with flags a string of u (up), l (loopback), m (multicast), b (broadcast),
 * p (point-to-point); returns 0 or an errno. */
SLANG_PLAT_HIDDEN int slang__plat_interfaces(char ***out, int *n);

/* ---- trust store (certroots.c; std.crypto.x509) ------------------------ */

/* Path of the system's PEM bundle of trusted root certificates
 * ($SSL_CERT_FILE first, then the distribution locations), or NULL if
 * none exists (and on systems whose roots are not files: stub). */
SLANG_PLAT_HIDDEN const char *slang__plat_cert_bundle_path(void);

#endif
