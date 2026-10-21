/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_FCNTL_H
#define SLANG_STUB_FCNTL_H
#include <sys/types.h>
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_ACCMODE 0x0003
#define O_NONBLOCK 0x00000004
#define O_APPEND 0x00000008
#define O_SHLOCK 0x00000010
#define O_EXLOCK 0x00000020
#define O_ASYNC 0x00000040
#define O_FSYNC 0x00000080
#define O_SYNC 0x0080
#define O_NOFOLLOW 0x00000100
#define O_CREAT 0x00000200
#define O_TRUNC 0x00000400
#define O_EXCL 0x00000800
#define O_NOCTTY 0x00020000
#define O_DIRECTORY 0x00100000
#define O_DSYNC 0x400000
#define O_CLOEXEC 0x01000000
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 7
#define F_SETLK 8
#define F_SETLKW 9
#define F_FULLFSYNC 51
#define F_DUPFD_CLOEXEC 67
#define F_NOCACHE 48
#define F_GETPATH 50
#define FD_CLOEXEC 1
#define F_RDLCK 1
#define F_UNLCK 2
#define F_WRLCK 3
#define AT_FDCWD -2
#define AT_EACCESS 0x0010
#define AT_SYMLINK_NOFOLLOW 0x0020
#define AT_SYMLINK_FOLLOW 0x0040
#define AT_REMOVEDIR 0x0080
struct flock { off_t l_start; off_t l_len; pid_t l_pid; short l_type; short l_whence; };
int open(const char *, int, ...);
int openat(int, const char *, int, ...);
int creat(const char *, mode_t);
int fcntl(int, int, ...);
int flock(int, int);
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#endif
