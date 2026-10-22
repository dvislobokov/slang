/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SYS_STATVFS_H
#define ZELL_STUB_SYS_STATVFS_H
#include <sys/types.h>
typedef unsigned int fsfilcnt_t;
struct statvfs { unsigned long f_bsize; unsigned long f_frsize; fsblkcnt_t f_blocks; fsblkcnt_t f_bfree; fsblkcnt_t f_bavail; fsfilcnt_t f_files; fsfilcnt_t f_ffree; fsfilcnt_t f_favail; unsigned long f_fsid; unsigned long f_flag; unsigned long f_namemax; };
int statvfs(const char *restrict, struct statvfs *restrict);
int fstatvfs(int, struct statvfs *);
#endif
