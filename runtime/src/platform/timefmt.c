/* timefmt.c - calendar arithmetic for every platform (UTC fields <-> Unix
 * seconds, platform.h) and, where the C library lacks them (Windows:
 * msvcrt's strftime knows only C89 conversions and aborts on others; there
 * is no strptime), a strftime/strptime with the C99/POSIX conversions in
 * the "C" locale. */
#include "platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool is_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

void slang__plat_utc_tm(int64_t t, struct tm *tm) {
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    memset(tm, 0, sizeof *tm);
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    int64_t wd = (days + 4) % 7;  /* 1970-01-01 was a Thursday */
    tm->tm_wday = (int)(wd < 0 ? wd + 7 : wd);
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = (int)(m - 1);
    tm->tm_mday = (int)d;
    tm->tm_yday = (int)(days - days_from_civil(y, 1, 1));
}

int64_t slang__plat_timegm(const struct tm *tm) {
    int64_t y = (int64_t)tm->tm_year + 1900, mon = tm->tm_mon;
    y += mon / 12;
    mon %= 12;
    if (mon < 0) { mon += 12; y--; }
    int64_t days = days_from_civil(y, mon + 1, 1) + tm->tm_mday - 1;
    return days * 86400 + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
}

/* ---- strftime / strptime in the "C" locale -------------------------------
 * Always compiled (the POSIX build tests them against the C library);
 * they are the platform's slang__plat_strftime/strptime on Windows and
 * musl (!SLANG_PLAT_LIBC_TIMEFMT). */

static const char *const g_days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const g_mons[] = {"January", "February", "March", "April", "May", "June",
                                     "July", "August", "September", "October", "November", "December"};

typedef struct out { char *b; size_t cap, n; bool over; } out;

static void put(out *o, const char *s, size_t n) {
    if (o->n + n >= o->cap) { o->over = true; return; }
    memcpy(o->b + o->n, s, n);
    o->n += n;
}
static void puts_(out *o, const char *s) { put(o, s, strlen(s)); }
static void putn(out *o, int64_t v, int width, char pad) {
    char t[32];
    int k = pad ? snprintf(t, sizeof t, pad == '0' ? "%0*lld" : "%*lld", width, (long long)v) : snprintf(t, sizeof t, "%lld", (long long)v);
    put(o, t, (size_t)k);
}

/* ISO 8601 week-based year and week of tm. */
static void iso_week(const struct tm *tm, int64_t *year, int *week) {
    int64_t y = (int64_t)tm->tm_year + 1900;
    int wday = (tm->tm_wday + 6) % 7;  /* Monday = 0 */
    int w = (tm->tm_yday - wday + 10) / 7;
    if (w < 1) {
        y--;
        int64_t pyd = tm->tm_yday + (is_leap(y) ? 366 : 365);
        w = (int)((pyd - wday + 10) / 7);
    } else if (w == 53) {
        int64_t len = is_leap(y) ? 366 : 365;
        if (tm->tm_yday - wday + 3 >= len) { y++; w = 1; }
    }
    *year = y;
    *week = w;
}

static void fmt(out *o, const char *f, const struct tm *tm) {
    for (; *f && !o->over; f++) {
        if (*f != '%' || !f[1]) { put(o, f, 1); continue; }
        char c = *++f;
        if (c == 'E' || c == 'O') c = *++f;  /* alternative representations: same in "C" */
        int64_t year = (int64_t)tm->tm_year + 1900;
        int h12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        switch (c) {
        case 'a': put(o, g_days[tm->tm_wday % 7], 3); break;
        case 'A': puts_(o, g_days[tm->tm_wday % 7]); break;
        case 'b': case 'h': put(o, g_mons[tm->tm_mon % 12], 3); break;
        case 'B': puts_(o, g_mons[tm->tm_mon % 12]); break;
        case 'c': fmt(o, "%a %b %e %H:%M:%S %Y", tm); break;
        case 'C': putn(o, year / 100, 2, '0'); break;
        case 'd': putn(o, tm->tm_mday, 2, '0'); break;
        case 'D': fmt(o, "%m/%d/%y", tm); break;
        case 'e': putn(o, tm->tm_mday, 2, ' '); break;
        case 'F': fmt(o, "%Y-%m-%d", tm); break;
        case 'g': case 'G': case 'V': {
            int64_t iy;
            int w;
            iso_week(tm, &iy, &w);
            if (c == 'V') putn(o, w, 2, '0');
            else if (c == 'g') putn(o, ((iy % 100) + 100) % 100, 2, '0');
            else putn(o, iy, 0, 0);
            break;
        }
        case 'H': putn(o, tm->tm_hour, 2, '0'); break;
        case 'I': putn(o, h12, 2, '0'); break;
        case 'j': putn(o, tm->tm_yday + 1, 3, '0'); break;
        case 'k': putn(o, tm->tm_hour, 2, ' '); break;
        case 'l': putn(o, h12, 2, ' '); break;
        case 'm': putn(o, tm->tm_mon + 1, 2, '0'); break;
        case 'M': putn(o, tm->tm_min, 2, '0'); break;
        case 'n': put(o, "\n", 1); break;
        case 'p': puts_(o, tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'P': puts_(o, tm->tm_hour < 12 ? "am" : "pm"); break;
        case 'r': fmt(o, "%I:%M:%S %p", tm); break;
        case 'R': fmt(o, "%H:%M", tm); break;
        case 's': putn(o, slang__plat_timegm(tm), 0, 0); break;
        case 'S': putn(o, tm->tm_sec, 2, '0'); break;
        case 't': put(o, "\t", 1); break;
        case 'T': fmt(o, "%H:%M:%S", tm); break;
        case 'u': putn(o, tm->tm_wday ? tm->tm_wday : 7, 0, 0); break;
        case 'U': putn(o, (tm->tm_yday + 7 - tm->tm_wday) / 7, 2, '0'); break;
        case 'w': putn(o, tm->tm_wday, 0, 0); break;
        case 'W': putn(o, (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7, 2, '0'); break;
        case 'x': fmt(o, "%m/%d/%y", tm); break;
        case 'X': fmt(o, "%H:%M:%S", tm); break;
        case 'y': putn(o, ((year % 100) + 100) % 100, 2, '0'); break;
        case 'Y': putn(o, year, 0, 0); break;
        case 'z': puts_(o, "+0000"); break;  /* calendar functions are UTC */
        case 'Z': puts_(o, "UTC"); break;
        case '%': put(o, "%", 1); break;
        default: put(o, f - 1, 2); break;
        }
    }
}

size_t slang__plat_strftime_portable(char *buf, size_t cap, const char *f, const struct tm *tm) {
    if (cap == 0) return 0;
    out o = { buf, cap, 0, false };
    fmt(&o, f, tm);
    if (o.over) return 0;
    buf[o.n] = 0;
    return o.n;
}

/* ---- strptime -------------------------------------------------------------- */

static const char *num(const char *s, int maxd, int64_t lo, int64_t hi, int *v) {
    while (*s == ' ') s++;
    int sign = 1;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    int64_t x = 0;
    int k = 0;
    while (k < maxd && isdigit((unsigned char)*s)) { x = x * 10 + (*s++ - '0'); k++; }
    if (k == 0) return NULL;
    x *= sign;
    if (x < lo || x > hi) return NULL;
    *v = (int)x;
    return s;
}

static const char *name(const char *s, const char *const *names, int count, int *v) {
    for (int i = 0; i < count; i++) {
        size_t full = strlen(names[i]);
        if (strncasecmp(s, names[i], full) == 0) { *v = i; return s + full; }
    }
    for (int i = 0; i < count; i++)
        if (strncasecmp(s, names[i], 3) == 0) { *v = i; return s + 3; }
    return NULL;
}

static const char *parse(const char *s, const char *f, struct tm *tm, long *gmtoff, int *century, int *pm) {
    for (; *f && s; f++) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*s)) s++;
            continue;
        }
        if (*f != '%' || !f[1]) {
            if (*s != *f) return NULL;
            s++;
            continue;
        }
        char c = *++f;
        if (c == 'E' || c == 'O') c = *++f;
        int v;
        switch (c) {
        case 'a': case 'A': s = name(s, g_days, 7, &tm->tm_wday); break;
        case 'b': case 'B': case 'h': s = name(s, g_mons, 12, &tm->tm_mon); break;
        case 'c': s = parse(s, "%a %b %e %H:%M:%S %Y", tm, gmtoff, century, pm); break;
        case 'C': if ((s = num(s, 2, 0, 99, &v))) *century = v; break;
        case 'd': case 'e': s = num(s, 2, 1, 31, &tm->tm_mday); break;
        case 'D': s = parse(s, "%m/%d/%y", tm, gmtoff, century, pm); break;
        case 'F': s = parse(s, "%Y-%m-%d", tm, gmtoff, century, pm); break;
        case 'H': case 'k': s = num(s, 2, 0, 23, &tm->tm_hour); break;
        case 'I': case 'l': s = num(s, 2, 1, 12, &tm->tm_hour); break;
        case 'j': if ((s = num(s, 3, 1, 366, &v))) tm->tm_yday = v - 1; break;
        case 'm': if ((s = num(s, 2, 1, 12, &v))) tm->tm_mon = v - 1; break;
        case 'M': s = num(s, 2, 0, 59, &tm->tm_min); break;
        case 'n': case 't': while (isspace((unsigned char)*s)) s++; break;
        case 'p':
            if (strncasecmp(s, "AM", 2) == 0) { *pm = 0; s += 2; }
            else if (strncasecmp(s, "PM", 2) == 0) { *pm = 1; s += 2; }
            else s = NULL;
            break;
        case 'r': s = parse(s, "%I:%M:%S %p", tm, gmtoff, century, pm); break;
        case 'R': s = parse(s, "%H:%M", tm, gmtoff, century, pm); break;
        case 'S': s = num(s, 2, 0, 61, &tm->tm_sec); break;
        case 'T': s = parse(s, "%H:%M:%S", tm, gmtoff, century, pm); break;
        case 'w': s = num(s, 1, 0, 6, &tm->tm_wday); break;
        case 'u': if ((s = num(s, 1, 1, 7, &v))) tm->tm_wday = v % 7; break;
        case 'x': s = parse(s, "%m/%d/%y", tm, gmtoff, century, pm); break;
        case 'X': s = parse(s, "%H:%M:%S", tm, gmtoff, century, pm); break;
        case 'y':
            if ((s = num(s, 2, 0, 99, &v))) tm->tm_year = v < 69 ? v + 100 : v;  /* POSIX: 69-99 -> 19xx */
            break;
        case 'Y': if ((s = num(s, 4, -9999, 9999, &v))) tm->tm_year = v - 1900; break;
        case 'z': {
            while (*s == ' ') s++;
            if (*s == 'Z') { *gmtoff = 0; s++; break; }
            int sign = *s == '-' ? -1 : *s == '+' ? 1 : 0;
            if (!sign) { s = NULL; break; }
            s++;
            int hh, mm = 0;
            if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) { s = NULL; break; }
            hh = (s[0] - '0') * 10 + (s[1] - '0');
            s += 2;
            if (*s == ':') s++;
            if (isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1])) { mm = (s[0] - '0') * 10 + (s[1] - '0'); s += 2; }
            *gmtoff = sign * (hh * 3600L + mm * 60L);
            break;
        }
        case 'Z': while (isalpha((unsigned char)*s)) s++; break;
        case '%': if (*s == '%') s++; else s = NULL; break;
        default: s = NULL; break;
        }
    }
    return s;
}

const char *slang__plat_strptime_portable(const char *s, const char *f, struct tm *tm, long *gmtoff) {
    int century = -1, pm = -1;
    *gmtoff = 0;
    const char *end = parse(s, f, tm, gmtoff, &century, &pm);
    if (!end) return NULL;
    if (century >= 0) tm->tm_year = century * 100 + tm->tm_year % 100 - 1900;
    if (pm >= 0) tm->tm_hour = tm->tm_hour % 12 + (pm ? 12 : 0);
    return end;
}

#if !defined(SLANG_PLAT_LIBC_TIMEFMT)
size_t slang__plat_strftime(char *buf, size_t cap, const char *f, const struct tm *tm) {
    return slang__plat_strftime_portable(buf, cap, f, tm);
}
const char *slang__plat_strptime(const char *s, const char *f, struct tm *tm, long *gmtoff) {
    return slang__plat_strptime_portable(s, f, tm, gmtoff);
}
#endif
