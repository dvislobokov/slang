/* std_time.c - time (zell_time_*). Times are int64 milliseconds since the
 * Unix epoch; calendar functions use UTC. Format/Parse layouts are strftime /
 * strptime patterns (plus `%L` = 3-digit milliseconds in Format). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int64_t zell_time_now(void) {
    int64_t ns = zell__plat_wall_ns();
    return ns / 1000000 - (ns % 1000000 < 0);
}

int64_t zell_time_now_nanos(void) { return zell__plat_wall_ns(); }

void zell_time_sleep(int64_t ms) {
    if (ms <= 0) return;
    zell__plat_sleep_ns(ms * 1000000);
}

int64_t zell_time_since(int64_t start_ms) { return zell_time_now() - start_ms; }

/* floor division / modulo so negative times map to the right calendar second */
static int64_t fdiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

static struct tm utc(int64_t ms) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    zell__plat_utc_tm(fdiv(ms, 1000), &tm);
    return tm;
}

int64_t zell_time_year(int64_t ms) { return (int64_t)utc(ms).tm_year + 1900; }
int64_t zell_time_month(int64_t ms) { return utc(ms).tm_mon + 1; }
int64_t zell_time_day(int64_t ms) { return utc(ms).tm_mday; }
int64_t zell_time_hour(int64_t ms) { return utc(ms).tm_hour; }
int64_t zell_time_minute(int64_t ms) { return utc(ms).tm_min; }
int64_t zell_time_second(int64_t ms) { return utc(ms).tm_sec; }
int64_t zell_time_weekday(int64_t ms) { return utc(ms).tm_wday; }

zell_string *zell_time_format(int64_t ms, const zell_string *layout) {
    struct tm tm = utc(ms);
    int msec = (int)(ms - fdiv(ms, 1000) * 1000);
    const char *l = zell_string_data(layout);
    int64_t ln = zell_string_len(layout);
    if (ln == 0) return zell__string_alloc(0);
    /* Expand %L ourselves; the rest goes to strftime. A leading space keeps a
     * legitimately empty strftime result distinguishable from failure. */
    char *pat = zell_alloc(ln * 2 + 2);
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
        char *buf = zell_alloc((int64_t)cap);
        size_t n = zell__plat_strftime(buf, cap, pat, &tm);
        if (n > 0) {
            zell_string *r = zell_string_new(buf + 1, (int64_t)n - 1);
            zell_free(buf);
            zell_free(pat);
            return r;
        }
        zell_free(buf);
        if (cap > ((size_t)1 << 24)) break;
        cap *= 4;
    }
    zell_free(pat);
    return zell__string_alloc(0);
}

int64_t zell_time_parse(const zell_string *s, const zell_string *layout, zell_error **err) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_mday = 1;
    tm.tm_year = 70;
    const char *p = zell_string_cstr(s);
    long off = 0;   /* set by %z, 0 otherwise */
    const char *end = zell__plat_strptime(p, zell_string_cstr(layout), &tm, &off);
    if (!end || *end != 0 || (int64_t)strlen(p) != zell_string_len(s)) {
        if (err) {
            char buf[256];
            snprintf(buf, sizeof buf, "time: cannot parse \"%.100s\" as \"%.100s\"", p,
                     zell_string_cstr(layout));
            *err = zell_error_new_cstr(buf);
        }
        return 0;
    }
    tm.tm_isdst = 0;
    int64_t t = zell__plat_timegm(&tm);
    if (err) *err = NULL;
    return (t - off) * 1000;
}
