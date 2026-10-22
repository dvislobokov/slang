/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SYS_SELECT_H
#define ZELL_STUB_SYS_SELECT_H
#include <sys/types.h>
#include <sys/time.h>
#define FD_SETSIZE 1024
typedef struct fd_set { __int32_t fds_bits[32]; } fd_set;
int select(int, fd_set *restrict, fd_set *restrict, fd_set *restrict, struct timeval *restrict);
#endif
