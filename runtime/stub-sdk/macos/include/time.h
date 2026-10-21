/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_TIME_H
#define SLANG_STUB_TIME_H
#include <sys/types.h>
struct timespec { __darwin_time_t tv_sec; long tv_nsec; };
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    char *tm_zone;
};
#define CLOCKS_PER_SEC 1000000
typedef enum {
    _CLOCK_REALTIME = 0,
    _CLOCK_MONOTONIC = 6,
    _CLOCK_MONOTONIC_RAW = 4,
    _CLOCK_MONOTONIC_RAW_APPROX = 5,
    _CLOCK_UPTIME_RAW = 8,
    _CLOCK_UPTIME_RAW_APPROX = 9,
    _CLOCK_PROCESS_CPUTIME_ID = 12,
    _CLOCK_THREAD_CPUTIME_ID = 16
} clockid_t;
#define CLOCK_REALTIME _CLOCK_REALTIME
#define CLOCK_MONOTONIC _CLOCK_MONOTONIC
#define CLOCK_MONOTONIC_RAW _CLOCK_MONOTONIC_RAW
#define CLOCK_UPTIME_RAW _CLOCK_UPTIME_RAW
#define CLOCK_PROCESS_CPUTIME_ID _CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID _CLOCK_THREAD_CPUTIME_ID
extern char *tzname[];
extern long timezone;
extern int daylight;
time_t time(time_t *);
clock_t clock(void);
double difftime(time_t, time_t);
time_t mktime(struct tm *);
time_t timegm(struct tm *);
time_t timelocal(struct tm *);
struct tm *gmtime(const time_t *);
struct tm *gmtime_r(const time_t *restrict, struct tm *restrict);
struct tm *localtime(const time_t *);
struct tm *localtime_r(const time_t *restrict, struct tm *restrict);
char *asctime(const struct tm *);
char *ctime(const time_t *);
size_t strftime(char *restrict, size_t, const char *restrict, const struct tm *restrict);
char *strptime(const char *restrict, const char *restrict, struct tm *restrict);
void tzset(void);
int nanosleep(const struct timespec *, struct timespec *);
int clock_gettime(clockid_t, struct timespec *);
int clock_getres(clockid_t, struct timespec *);
uint64_t clock_gettime_nsec_np(clockid_t);
#endif
