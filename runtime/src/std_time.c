/* std_time.c - std.time (slang_time_*). Times are int64 milliseconds since the
 * Unix epoch; calendar functions use UTC. Format/Parse layouts are strftime /
 * strptime patterns (plus `%L` = 3-digit milliseconds in Format). */
#include "rt_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int64_t slang_time_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t slang_time_now_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void slang_time_sleep(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

int64_t slang_time_since(int64_t start_ms) { return slang_time_now() - start_ms; }

/* floor division / modulo so negative times map to the right calendar second */
static int64_t fdiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

static struct tm utc(int64_t ms) {
    time_t t = (time_t)fdiv(ms, 1000);
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (!gmtime_r(&t, &tm)) memset(&tm, 0, sizeof tm);
    return tm;
}

int64_t slang_time_year(int64_t ms) { return (int64_t)utc(ms).tm_year + 1900; }
int64_t slang_time_month(int64_t ms) { return utc(ms).tm_mon + 1; }
int64_t slang_time_day(int64_t ms) { return utc(ms).tm_mday; }
int64_t slang_time_hour(int64_t ms) { return utc(ms).tm_hour; }
int64_t slang_time_minute(int64_t ms) { return utc(ms).tm_min; }
int64_t slang_time_second(int64_t ms) { return utc(ms).tm_sec; }
int64_t slang_time_weekday(int64_t ms) { return utc(ms).tm_wday; }

slang_string *slang_time_format(int64_t ms, const slang_string *layout) {
    struct tm tm = utc(ms);
    int msec = (int)(ms - fdiv(ms, 1000) * 1000);
    const char *l = slang_string_data(layout);
    int64_t ln = slang_string_len(layout);
    if (ln == 0) return slang__string_alloc(0);
    /* Expand %L ourselves; the rest goes to strftime. A leading space keeps a
     * legitimately empty strftime result distinguishable from failure. */
    char *pat = slang_alloc(ln * 2 + 2);
    int64_t k = 0;
    pat[k++] = ' ';
    for (int64_t i = 0; i < ln; i++) {
        if (l[i] == '%' && i + 1 < ln && l[i + 1] == 'L') {
            k += snprintf(pat + k, 4, "%03d", msec);
            i++;
        } else if (l[i] == '%' && i + 1 < ln) {
            pat[k++] = l[i++];
            pat[k++] = l[i];
        } else {
            pat[k++] = l[i];
        }
    }
    pat[k] = 0;
    size_t cap = (size_t)k * 4 + 64;
    for (;;) {
        char *buf = slang_alloc((int64_t)cap);
        size_t n = strftime(buf, cap, pat, &tm);
        if (n > 0) {
            slang_string *r = slang_string_new(buf + 1, (int64_t)n - 1);
            slang_free(buf);
            slang_free(pat);
            return r;
        }
        slang_free(buf);
        if (cap > ((size_t)1 << 24)) break;
        cap *= 4;
    }
    slang_free(pat);
    return slang__string_alloc(0);
}

int64_t slang_time_parse(const slang_string *s, const slang_string *layout, slang_error **err) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_mday = 1;
    tm.tm_year = 70;
    const char *p = slang_string_data(s);
    const char *end = strptime(p, slang_string_data(layout), &tm);
    if (!end || *end != 0 || (int64_t)strlen(p) != slang_string_len(s)) {
        if (err) {
            char buf[256];
            snprintf(buf, sizeof buf, "time: cannot parse \"%.100s\" as \"%.100s\"", p,
                     slang_string_data(layout));
            *err = slang_error_new_cstr(buf);
        }
        return 0;
    }
    long off = tm.tm_gmtoff;   /* set by %z (glibc), 0 otherwise */
    tm.tm_isdst = 0;
    time_t t = timegm(&tm);
    if (err) *err = NULL;
    return ((int64_t)t - off) * 1000;
}
