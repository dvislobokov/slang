/* platform.h - the runtime's internal platform layer (docs/PLATFORMS.md).
 *
 * Everything OS-specific that the rest of the runtime needs goes through
 * these functions; the implementations live in runtime/src/platform/:
 *   posix.c        threads, CPU count, monotonic clock, entropy, backtraces
 *   poll_epoll.c   readiness poller on Linux (epoll + eventfd)
 *   poll_kqueue.c  readiness poller on macOS/BSD (kqueue + EVFILT_USER)
 *   net_posix.c    BSD sockets (TCP/UDP/Unix, options, sendfile), names
 * Not part of the ABI: every symbol is hidden and prefixed slang__plat_.
 * (Mutexes and condition variables are still used directly as pthreads in
 * task.c; a Win32 port wraps them here too.) */
#ifndef SLANG_PLATFORM_H
#define SLANG_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SLANG_PLAT_HIDDEN __attribute__((visibility("hidden")))

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

/* ---- time and entropy ------------------------------------------------- */

/* Monotonic clock in nanoseconds (arbitrary origin). */
SLANG_PLAT_HIDDEN int64_t slang__plat_mono_ns(void);
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
