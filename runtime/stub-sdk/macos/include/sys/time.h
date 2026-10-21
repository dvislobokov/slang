/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_SYS_TIME_H
#define SLANG_STUB_SYS_TIME_H
#include <sys/types.h>
#include <time.h>
struct timeval { __darwin_time_t tv_sec; __darwin_suseconds_t tv_usec; };
struct itimerval { struct timeval it_interval; struct timeval it_value; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2
int gettimeofday(struct timeval *restrict, void *restrict);
int setitimer(int, const struct itimerval *restrict, struct itimerval *restrict);
int getitimer(int, struct itimerval *);
int utimes(const char *, const struct timeval[2]);
int futimes(int, const struct timeval[2]);
#endif
