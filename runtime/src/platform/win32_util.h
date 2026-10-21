/* win32_util.h - helpers shared by the Windows files of the platform layer
 * (win32.c, fs_win32.c, proc_win32.c, net_win32.c, poll_iocp.c,
 * poll_wsapoll.c, prof_win32.c, tz_win32.c, certroots.c, sqlite_win32.c). Only
 * included on _WIN32. */
#ifndef SLANG_WIN32_UTIL_H
#define SLANG_WIN32_UTIL_H

#if !defined(_WIN32)
#error "win32_util.h is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "platform.h"

/* malloc'd UTF-16 copy of a UTF-8 string (NULL on invalid input or OOM). */
SLANG_PLAT_HIDDEN wchar_t *slang__w32_wide(const char *s);
/* The same for a path: '/' becomes '\\' (Win32 accepts both, but the
 * \\?\ form and some shell APIs do not). */
SLANG_PLAT_HIDDEN wchar_t *slang__w32_wpath(const char *s);
/* malloc'd UTF-8 copy of a UTF-16 string (n = -1: NUL-terminated). */
SLANG_PLAT_HIDDEN char *slang__w32_utf8(const wchar_t *w, int n);
/* POSIX errno of a GetLastError() code. */
SLANG_PLAT_HIDDEN int slang__w32_errno(DWORD err);
/* Writes UTF-8 text to a console handle with WriteConsoleW; carry holds an
 * incomplete trailing sequence between calls (4 bytes + length byte). */
SLANG_PLAT_HIDDEN void slang__w32_console_write(HANDLE h, unsigned char carry[5], const char *p, size_t n);
/* True if CRT descriptor fd (1 or 2) is a console (set at runtime init). */
SLANG_PLAT_HIDDEN bool slang__w32_is_console_fd(int64_t fd);
/* Console writer for fd 1/2 (shares the carry buffer with stdio output). */
SLANG_PLAT_HIDDEN void slang__w32_console_fd_write(int64_t fd, const char *p, size_t n);
/* WSAStartup once. */
SLANG_PLAT_HIDDEN void slang__w32_wsa_init(void);
/* A connected pair of loopback TCP sockets (not inherited); 0 or a WSA error. */
SLANG_PLAT_HIDDEN int slang__w32_socketpair(SOCKET sv[2]);

/* Child-process pipes of proc_win32.c for poll_iocp.c: the parent's handle
 * of pipe descriptor fd and its direction (false: not such a pipe); whether
 * its write is still in flight; whether ov is the OVERLAPPED of a pipe's
 * write (and which descriptor). */
SLANG_PLAT_HIDDEN bool slang__w32_pipe_of(int64_t fd, HANDLE *h, bool *write);
SLANG_PLAT_HIDDEN bool slang__w32_pipe_write_busy(int64_t fd);
SLANG_PLAT_HIDDEN bool slang__w32_pipe_write_ov(const void *ov, int64_t *fd);
/* True if the pollers are IOCP ones, which can wait on named pipes
 * (proc_win32.c then uses them instead of relay threads). */
SLANG_PLAT_HIDDEN bool slang__w32_poller_waits_pipes(void);

/* The WSAPoll poller (poll_wsapoll.c), the fallback of poll_iocp.c; same
 * contract as slang__plat_poller_*. */
typedef struct slang__w32_wsapoll slang__w32_wsapoll;
SLANG_PLAT_HIDDEN slang__w32_wsapoll *slang__w32_wsapoll_new(void);
SLANG_PLAT_HIDDEN void slang__w32_wsapoll_free(slang__w32_wsapoll *p);
SLANG_PLAT_HIDDEN int slang__w32_wsapoll_arm(slang__w32_wsapoll *p, int64_t fd, int32_t interest, uint64_t token, bool registered);
SLANG_PLAT_HIDDEN void slang__w32_wsapoll_remove(slang__w32_wsapoll *p, int64_t fd);
SLANG_PLAT_HIDDEN int slang__w32_wsapoll_wait(slang__w32_wsapoll *p, slang__plat_event *evs, int max, int64_t timeout_ms);
SLANG_PLAT_HIDDEN void slang__w32_wsapoll_wake(slang__w32_wsapoll *p);

#endif
