/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_SYS_TYPES_H
#define SLANG_STUB_SYS_TYPES_H
#include <stddef.h>
#include <stdint.h>
#include <sys/_types.h>
typedef __darwin_ssize_t ssize_t;
typedef __darwin_off_t off_t;
typedef __darwin_pid_t pid_t;
typedef __darwin_uid_t uid_t;
typedef __darwin_gid_t gid_t;
typedef __darwin_mode_t mode_t;
typedef __darwin_dev_t dev_t;
typedef __darwin_ino64_t ino_t;
typedef __uint16_t nlink_t;
typedef __int32_t blksize_t;
typedef __int64_t blkcnt_t;
typedef __darwin_time_t time_t;
typedef __darwin_suseconds_t suseconds_t;
typedef __darwin_useconds_t useconds_t;
typedef __uint32_t id_t;
typedef unsigned long clock_t;
typedef __uint32_t fsblkcnt_t;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef __int32_t key_t;
typedef char *caddr_t;
typedef __uint8_t u_int8_t;
typedef __uint16_t u_int16_t;
typedef __uint32_t u_int32_t;
typedef __uint64_t u_int64_t;
#include <sys/_pthread_types.h>
#endif
