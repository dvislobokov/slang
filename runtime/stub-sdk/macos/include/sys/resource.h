/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SYS_RESOURCE_H
#define ZELL_STUB_SYS_RESOURCE_H
#include <sys/types.h>
#include <sys/time.h>
typedef __uint64_t rlim_t;
struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; };
struct rusage { struct timeval ru_utime; struct timeval ru_stime; long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap, ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw; };
#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_AS 5
#define RLIMIT_NOFILE 8
#define RLIM_INFINITY (((__uint64_t)1 << 63) - 1)
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN -1
#define PRIO_PROCESS 0
int getrlimit(int, struct rlimit *);
int setrlimit(int, const struct rlimit *);
int getrusage(int, struct rusage *);
int getpriority(int, id_t);
int setpriority(int, id_t, int);
#endif
