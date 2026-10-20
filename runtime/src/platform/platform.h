/* platform.h - the runtime's internal platform layer (docs/PLATFORMS.md).
 *
 * Everything OS-specific that the rest of the runtime needs goes through
 * these functions; the implementations live in runtime/src/platform/:
 *   posix.c        threads, CPU count, monotonic clock, entropy, backtraces
 *   poll_epoll.c   readiness poller on Linux (epoll + eventfd)
 *   poll_kqueue.c  readiness poller on macOS/BSD (not implemented yet)
 *   net_posix.c    BSD sockets (non-blocking TCP/UDP) and name resolution
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

/* ---- readiness poller (async I/O backend) ----------------------------- */

enum { SLANG_PLAT_READ = 1, SLANG_PLAT_WRITE = 2, SLANG_PLAT_ERROR = 4 };

typedef struct slang__plat_event {
    int64_t fd;      /* the descriptor, or -1 for the poller's own wake-up */
    int32_t events;  /* SLANG_PLAT_READ | SLANG_PLAT_WRITE | SLANG_PLAT_ERROR */
} slang__plat_event;

typedef struct slang__plat_poller slang__plat_poller;

/* A new poller with a wake-up channel; NULL on failure. */
SLANG_PLAT_HIDDEN slang__plat_poller *slang__plat_poller_new(void);
SLANG_PLAT_HIDDEN void slang__plat_poller_free(slang__plat_poller *p);
/* Sets the interest of fd (READ/WRITE mask, 0 = none but stay registered).
 * `registered` says whether fd was already added. Returns 0 or an errno. */
SLANG_PLAT_HIDDEN int slang__plat_poller_set(slang__plat_poller *p, int64_t fd, int32_t interest, bool registered);
/* Removes fd (before it is closed). */
SLANG_PLAT_HIDDEN void slang__plat_poller_remove(slang__plat_poller *p, int64_t fd);
/* Waits up to timeout_ms (-1 = forever) and stores up to max events.
 * Returns the number of events (0 on timeout or EINTR). The wake-up
 * channel shows up as fd -1 and is drained internally. */
SLANG_PLAT_HIDDEN int slang__plat_poller_wait(slang__plat_poller *p, slang__plat_event *evs, int max, int64_t timeout_ms);
/* Makes a concurrent or the next poller_wait return (thread-safe). */
SLANG_PLAT_HIDDEN void slang__plat_poller_wake(slang__plat_poller *p);

/* ---- sockets (net_posix.c) --------------------------------------------
 * Descriptors are int64_t. Functions returning int64_t give a value >= 0 or
 * -errno; SLANG_PLAT_EAGAIN is the (negated) "would block" code. All sockets
 * are non-blocking and close-on-exec. Addresses are numeric IPv4/IPv6
 * strings (no brackets). */

#define SLANG_PLAT_EAGAIN 100000  /* returned as -SLANG_PLAT_EAGAIN (not an errno value) */

SLANG_PLAT_HIDDEN int64_t slang__plat_tcp_connect(const char *ip, int64_t port);   /* connect started */
SLANG_PLAT_HIDDEN int     slang__plat_tcp_connect_result(int64_t fd);              /* 0 or errno */
SLANG_PLAT_HIDDEN int64_t slang__plat_tcp_listen(const char *ip, int64_t port, int backlog);
SLANG_PLAT_HIDDEN int64_t slang__plat_tcp_accept(int64_t fd);
SLANG_PLAT_HIDDEN int64_t slang__plat_udp_bind(const char *ip, int64_t port);
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_recv(int64_t fd, void *buf, int64_t n);  /* 0 = EOF */
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_send(int64_t fd, const void *buf, int64_t n);
/* UDP: from/to are "ip" + port. */
SLANG_PLAT_HIDDEN int64_t slang__plat_udp_recvfrom(int64_t fd, void *buf, int64_t n, char *from, size_t cap, int64_t *port);
SLANG_PLAT_HIDDEN int64_t slang__plat_udp_sendto(int64_t fd, const void *buf, int64_t n, const char *ip, int64_t port);
SLANG_PLAT_HIDDEN void    slang__plat_sock_close(int64_t fd);
/* "ip:port" ("[ip]:port" for IPv6) of the local or peer end; false on error. */
SLANG_PLAT_HIDDEN bool    slang__plat_sock_addr(int64_t fd, bool peer, char *buf, size_t cap);
SLANG_PLAT_HIDDEN int64_t slang__plat_sock_port(int64_t fd);
/* True if s is a numeric IPv4 or IPv6 address. */
SLANG_PLAT_HIDDEN bool    slang__plat_is_ip(const char *s);
/* Resolves host (blocking): on success *out is a malloc'd array of n
 * malloc'd numeric address strings (IPv4 first); returns 0, else an error
 * message for the name lookup (static storage). */
SLANG_PLAT_HIDDEN const char *slang__plat_resolve(const char *host, char ***out, int *n);
/* Message for an errno-style code. */
SLANG_PLAT_HIDDEN const char *slang__plat_strerror(int code, char *buf, size_t cap);

#endif
