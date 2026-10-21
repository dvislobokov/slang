/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_SYS_UIO_H
#define SLANG_STUB_SYS_UIO_H
#include <sys/types.h>
struct iovec { void *iov_base; size_t iov_len; };
#define IOV_MAX 1024
ssize_t readv(int, const struct iovec *, int);
ssize_t writev(int, const struct iovec *, int);
ssize_t preadv(int, const struct iovec *, int, off_t);
ssize_t pwritev(int, const struct iovec *, int, off_t);
#endif
